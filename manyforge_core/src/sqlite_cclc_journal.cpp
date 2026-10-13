#include <sqlite3.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <manyforge_content_identity/canonical_value.hpp>
#include <manyforge_content_identity/content_identity.hpp>
#include <manyforge_core/sqlite_cclc_journal.hpp>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <tuple>
#include <utility>
#include <vector>

#include "component_lifecycle_journal_codec_internal.hpp"

namespace manyforge::core {
namespace {

constexpr int kBusyTimeoutMilliseconds = 250;
constexpr std::int64_t kMaximumDatabasePageImageBytes = 8LL * 1024LL * 1024LL * 1024LL;
// SQLite's journal_size_limit controls the retained WAL size after a reset or
// checkpoint; it is not a hard bound on the live WAL while one transaction is
// committing. Keep enough retained headroom for one maximum logical status
// plus one maximum aggregate effect-history transition and SQLite framing.
constexpr std::int64_t kMaximumRetainedWalBytes = 512LL * 1024LL * 1024LL;
constexpr std::size_t kMaximumSqliteEncodedRowBytes =
    detail::kMaximumComponentLifecycleJournalRecordBytes + 1U * 1024U * 1024U;

constexpr std::string_view kSchema = R"sql(
CREATE TABLE cclc_journal_metadata (
  singleton INTEGER PRIMARY KEY CHECK (singleton = 1),
  schema_id TEXT NOT NULL,
  schema_version INTEGER NOT NULL,
  cell_id TEXT NOT NULL,
  exact_result_history_capacity INTEGER NOT NULL,
  terminal_result_bytes_capacity INTEGER NOT NULL,
  highest_transaction_generation INTEGER NOT NULL,
  highest_route_selection_transaction_generation INTEGER NOT NULL,
  highest_terminal_sequence INTEGER NOT NULL,
  observer_latest_bring_up BLOB CHECK (observer_latest_bring_up IS NULL OR
    length(observer_latest_bring_up) BETWEEN 1 AND 524288)
) STRICT;
CREATE TABLE cclc_owner_epochs (
  epoch_sequence INTEGER PRIMARY KEY AUTOINCREMENT,
  owner_epoch TEXT NOT NULL UNIQUE,
  started_at_ns INTEGER NOT NULL,
  request_membership_capacity INTEGER NOT NULL,
  active INTEGER NOT NULL CHECK (active IN (0, 1))
) STRICT;
CREATE UNIQUE INDEX one_active_cclc_owner_epoch
  ON cclc_owner_epochs(active) WHERE active = 1;
CREATE TABLE cclc_request_memberships (
  membership_sequence INTEGER PRIMARY KEY AUTOINCREMENT,
  owner_epoch TEXT NOT NULL,
  request_id TEXT NOT NULL,
  request_kind TEXT NOT NULL CHECK (
    request_kind IN ('component_lifecycle', 'route_selection', 'lifecycle_target_retirement')),
  semantic_request_digest TEXT NOT NULL,
  retained_result INTEGER NOT NULL CHECK (retained_result IN (0, 1)),
  target_retirement_blob BLOB,
  target_retirement_sequence INTEGER,
  UNIQUE(owner_epoch, request_id),
  FOREIGN KEY(owner_epoch) REFERENCES cclc_owner_epochs(owner_epoch)
) STRICT;
CREATE TABLE lifecycle_component_watermarks (
  controllable_component_id TEXT PRIMARY KEY,
  component_lifecycle_generation INTEGER NOT NULL
) STRICT;
CREATE TABLE lifecycle_component_current_facts (
  controllable_component_id TEXT PRIMARY KEY,
  component_lifecycle_generation INTEGER NOT NULL,
  component_lifecycle_state INTEGER NOT NULL CHECK (
    component_lifecycle_state BETWEEN 0 AND 7),
  FOREIGN KEY(controllable_component_id)
    REFERENCES lifecycle_component_watermarks(controllable_component_id)
) STRICT;
CREATE TABLE cclc_lifecycle_transaction_identity_guards (
  lifecycle_transaction_id TEXT PRIMARY KEY,
  lifecycle_transaction_generation INTEGER NOT NULL UNIQUE
) STRICT;
CREATE TABLE lifecycle_transactions (
  transaction_sequence INTEGER PRIMARY KEY AUTOINCREMENT,
  membership_sequence INTEGER NOT NULL UNIQUE,
  lifecycle_transaction_id TEXT NOT NULL UNIQUE,
  lifecycle_transaction_generation INTEGER NOT NULL UNIQUE,
  controllable_component_id TEXT NOT NULL,
  component_lifecycle_generation INTEGER NOT NULL,
  status_sequence INTEGER NOT NULL,
  accepted_at_ns INTEGER NOT NULL,
  latest_updated_at_ns INTEGER NOT NULL,
  terminal INTEGER NOT NULL CHECK (terminal IN (0, 1)),
  terminal_sequence INTEGER UNIQUE,
  status_blob BLOB NOT NULL,
  CHECK ((terminal = 0 AND terminal_sequence IS NULL) OR
         (terminal = 1 AND terminal_sequence IS NOT NULL)),
  FOREIGN KEY(membership_sequence)
    REFERENCES cclc_request_memberships(membership_sequence)
) STRICT;
CREATE INDEX lifecycle_transaction_terminal_order
  ON lifecycle_transactions(terminal, terminal_sequence);
CREATE TABLE cclc_lifecycle_cell_claims (
  lifecycle_transaction_id TEXT PRIMARY KEY,
  owner_epoch TEXT NOT NULL,
  request_id TEXT NOT NULL,
  request_payload_digest TEXT NOT NULL,
  expected_admission_generation INTEGER NOT NULL,
  claim_generation INTEGER,
  acquired_admission_generation INTEGER,
  accepted_at_ns INTEGER,
  lifecycle_accepted INTEGER NOT NULL DEFAULT 0 CHECK(lifecycle_accepted IN (0,1)),
  release_kind INTEGER,
  release_evidence_digest TEXT,
  release_receipt_recorded INTEGER NOT NULL DEFAULT 0
    CHECK(release_receipt_recorded IN (0,1)),
  CHECK ((claim_generation IS NULL AND acquired_admission_generation IS NULL
          AND accepted_at_ns IS NULL) OR
         (claim_generation IS NOT NULL AND acquired_admission_generation IS NOT NULL
          AND accepted_at_ns IS NOT NULL)),
  CHECK ((release_kind IS NULL AND release_evidence_digest IS NULL) OR
         (release_kind IS NOT NULL AND release_evidence_digest IS NOT NULL)),
  FOREIGN KEY(owner_epoch) REFERENCES cclc_owner_epochs(owner_epoch)
) STRICT;
CREATE TABLE cclc_effect_attempts (
  attempt_sequence INTEGER PRIMARY KEY AUTOINCREMENT,
  lifecycle_transaction_id TEXT NOT NULL,
  effect_sequence INTEGER NOT NULL,
  effect_kind TEXT NOT NULL CHECK (
    effect_kind IN ('brand_lifecycle_primitive')),
  effect_identity TEXT NOT NULL UNIQUE,
  prepared_owner_epoch TEXT NOT NULL,
  prepared_status_sequence INTEGER NOT NULL,
  prepared_status_digest TEXT NOT NULL,
  prepared_attempt_blob BLOB NOT NULL,
  completed_owner_epoch TEXT,
  completed_status_sequence INTEGER,
  completed_status_digest TEXT,
  completed_attempt_blob BLOB,
  CHECK ((completed_attempt_blob IS NULL AND completed_owner_epoch IS NULL AND
          completed_status_sequence IS NULL AND completed_status_digest IS NULL) OR
         (completed_attempt_blob IS NOT NULL AND completed_owner_epoch IS NOT NULL AND
          completed_status_sequence IS NOT NULL AND completed_status_digest IS NOT NULL)),
  UNIQUE(lifecycle_transaction_id, effect_sequence),
  FOREIGN KEY(lifecycle_transaction_id)
    REFERENCES lifecycle_transactions(lifecycle_transaction_id) ON DELETE CASCADE,
  FOREIGN KEY(prepared_owner_epoch) REFERENCES cclc_owner_epochs(owner_epoch),
  FOREIGN KEY(completed_owner_epoch) REFERENCES cclc_owner_epochs(owner_epoch)
) STRICT;
CREATE INDEX cclc_effect_attempt_transaction_order
  ON cclc_effect_attempts(lifecycle_transaction_id, effect_sequence);
CREATE TABLE control_domain_active_configuration_watermarks (
  control_domain_id TEXT PRIMARY KEY,
  active_control_configuration_generation INTEGER NOT NULL
) STRICT;
CREATE TABLE cclc_route_transaction_identity_guards (
  route_selection_transaction_id TEXT PRIMARY KEY,
  route_selection_transaction_generation INTEGER NOT NULL UNIQUE
) STRICT;
CREATE TABLE control_domain_active_configurations (
  control_domain_id TEXT PRIMARY KEY,
  active_control_configuration_generation INTEGER NOT NULL,
  origin_route_selection_transaction_id TEXT,
  origin_route_selection_transaction_generation INTEGER,
  configuration_blob BLOB NOT NULL,
  CHECK ((origin_route_selection_transaction_id IS NULL AND
          origin_route_selection_transaction_generation IS NULL) OR
         (origin_route_selection_transaction_id IS NOT NULL AND
          origin_route_selection_transaction_generation IS NOT NULL)),
  FOREIGN KEY(control_domain_id)
    REFERENCES control_domain_active_configuration_watermarks(control_domain_id),
  FOREIGN KEY(origin_route_selection_transaction_id)
    REFERENCES cclc_route_transaction_identity_guards(route_selection_transaction_id)
) STRICT;
CREATE TABLE route_selection_transactions (
  transaction_sequence INTEGER PRIMARY KEY AUTOINCREMENT,
  membership_sequence INTEGER NOT NULL UNIQUE,
  route_selection_transaction_id TEXT NOT NULL UNIQUE,
  route_selection_transaction_generation INTEGER NOT NULL UNIQUE,
  control_domain_id TEXT NOT NULL,
  active_control_configuration_generation INTEGER NOT NULL,
  status_sequence INTEGER NOT NULL,
  accepted_at_ns INTEGER NOT NULL,
  latest_updated_at_ns INTEGER NOT NULL,
  terminal INTEGER NOT NULL CHECK (terminal IN (0, 1)),
  terminal_sequence INTEGER UNIQUE,
  status_blob BLOB NOT NULL,
  CHECK ((terminal = 0 AND terminal_sequence IS NULL) OR
         (terminal = 1 AND terminal_sequence IS NOT NULL)),
  FOREIGN KEY(membership_sequence)
    REFERENCES cclc_request_memberships(membership_sequence)
) STRICT;
CREATE INDEX route_selection_transaction_terminal_order
  ON route_selection_transactions(terminal, terminal_sequence);
CREATE TABLE cclc_route_effect_attempts (
  attempt_sequence INTEGER PRIMARY KEY AUTOINCREMENT,
  route_selection_transaction_id TEXT NOT NULL,
  effect_sequence INTEGER NOT NULL,
  effect_kind TEXT NOT NULL CHECK (
    effect_kind IN ('kernel_route_configuration_fence_acquire',
                    'controller_lifecycle_transition',
                    'kernel_route_configuration_fence_release')),
  effect_identity TEXT NOT NULL UNIQUE,
  prepared_owner_epoch TEXT NOT NULL,
  prepared_status_sequence INTEGER NOT NULL,
  prepared_status_digest TEXT NOT NULL,
  prepared_attempt_blob BLOB NOT NULL,
  completed_owner_epoch TEXT,
  completed_status_sequence INTEGER,
  completed_status_digest TEXT,
  completed_attempt_blob BLOB,
  CHECK ((completed_attempt_blob IS NULL AND completed_owner_epoch IS NULL AND
          completed_status_sequence IS NULL AND completed_status_digest IS NULL) OR
         (completed_attempt_blob IS NOT NULL AND completed_owner_epoch IS NOT NULL AND
          completed_status_sequence IS NOT NULL AND completed_status_digest IS NOT NULL)),
  UNIQUE(route_selection_transaction_id, effect_sequence),
  FOREIGN KEY(route_selection_transaction_id)
    REFERENCES route_selection_transactions(route_selection_transaction_id) ON DELETE CASCADE,
  FOREIGN KEY(prepared_owner_epoch) REFERENCES cclc_owner_epochs(owner_epoch),
  FOREIGN KEY(completed_owner_epoch) REFERENCES cclc_owner_epochs(owner_epoch)
) STRICT;
CREATE INDEX cclc_route_effect_attempt_transaction_order
  ON cclc_route_effect_attempts(route_selection_transaction_id, effect_sequence);
)sql";

class JournalFailure final : public std::runtime_error {
 public:
  JournalFailure(std::string code, std::string message,
                 std::string reason = "component_lifecycle_journal_failure")
      : std::runtime_error(std::move(message)), code_(std::move(code)), reason_(std::move(reason)) {}

  [[nodiscard]] const std::string& code() const noexcept { return code_; }
  [[nodiscard]] const std::string& reason() const noexcept { return reason_; }

 private:
  std::string code_;
  std::string reason_;
};

[[noreturn]] void Fail(std::string code, std::string message) {
  throw JournalFailure(std::move(code), std::move(message));
}

void Require(bool condition, std::string code, std::string message) {
  if (!condition) {
    Fail(std::move(code), std::move(message));
  }
}

class CellClosePrerequisiteFailure final : public std::runtime_error {
 public:
  explicit CellClosePrerequisiteFailure(ErrorDTO error)
      : std::runtime_error(error.message), error_(std::move(error)) {}
  [[nodiscard]] const ErrorDTO& error() const noexcept { return error_; }
 private:
  ErrorDTO error_;
};

// Only typed Close retirement may report this dedicated uncertainty. Its
// complete value is allocated before writes and moved through the exception
// boundary; ordinary journal errors retain their existing classification.
struct TargetRetirementCommitUnknown final {
  ErrorDTO error;
};

[[nodiscard]] ErrorDTO JournalError(const JournalFailure& error) {
  return ErrorDTO{error.code(), error.what(), {{"reason", error.reason()}}};
}

template <typename Value>
[[nodiscard]] Result<Value> AsResult(auto&& function) {
  try {
    if constexpr (std::is_void_v<Value>) {
      function();
      return Result<void>::Ok();
    } else {
      return Result<Value>::Ok(function());
    }
  } catch (TargetRetirementCommitUnknown& error) {
    return Result<Value>::Err(std::move(error.error));
  } catch (const CellClosePrerequisiteFailure& error) {
    return Result<Value>::Err(error.error());
  } catch (const JournalFailure& error) {
    return Result<Value>::Err(JournalError(error));
  } catch (const std::exception& error) {
    return Result<Value>::Err(ErrorDTO{"component_lifecycle_journal_internal_failure",
                                       error.what(),
                                       {{"reason", "component_lifecycle_journal_exception"}}});
  }
}

[[nodiscard]] std::string SqliteDetail(sqlite3* database, int result) {
  return database == nullptr ? sqlite3_errstr(result) : sqlite3_errmsg(database);
}

void Check(sqlite3* database, int result, std::string_view operation) {
  if (result != SQLITE_OK) {
    Fail(result == SQLITE_BUSY || result == SQLITE_LOCKED
             ? "component_lifecycle_journal_busy"
             : "component_lifecycle_journal_storage_failure",
         std::string(operation) + ": " + SqliteDetail(database, result));
  }
}

void Exec(sqlite3* database, std::string_view sql) {
  char* error = nullptr;
  const std::string statement(sql);
  const auto result = sqlite3_exec(database, statement.c_str(), nullptr, nullptr, &error);
  if (result != SQLITE_OK) {
    const std::string detail = error == nullptr ? SqliteDetail(database, result) : error;
    sqlite3_free(error);
    Fail(result == SQLITE_BUSY || result == SQLITE_LOCKED
             ? "component_lifecycle_journal_busy"
             : "component_lifecycle_journal_storage_failure",
         "execute SQLite statement: " + detail);
  }
}

class Statement final {
 public:
  Statement(sqlite3* database, std::string_view sql) : database_(database) {
    Check(database_,
          sqlite3_prepare_v3(database_, sql.data(), static_cast<int>(sql.size()),
                             SQLITE_PREPARE_PERSISTENT, &statement_, nullptr),
          "prepare SQLite statement");
  }

  ~Statement() { sqlite3_finalize(statement_); }
  Statement(const Statement&) = delete;
  Statement& operator=(const Statement&) = delete;

  void Bind(int index, std::string_view value) {
    Check(database_,
          sqlite3_bind_text(statement_, index, value.data(), static_cast<int>(value.size()),
                            SQLITE_TRANSIENT),
          "bind SQLite text");
  }

  void Bind(int index, std::int64_t value) {
    Check(database_, sqlite3_bind_int64(statement_, index, value), "bind SQLite integer");
  }

  void BindBlob(int index, std::string_view value) {
    Check(database_,
          sqlite3_bind_blob64(statement_, index, value.data(), value.size(), SQLITE_TRANSIENT),
          "bind SQLite blob");
  }

  void BindNull(int index) {
    Check(database_, sqlite3_bind_null(statement_, index), "bind SQLite null");
  }

  [[nodiscard]] bool Step() {
    const auto result = sqlite3_step(statement_);
    if (result == SQLITE_ROW) {
      return true;
    }
    if (result == SQLITE_DONE) {
      return false;
    }
    Fail(result == SQLITE_BUSY || result == SQLITE_LOCKED
             ? "component_lifecycle_journal_busy"
             : "component_lifecycle_journal_storage_failure",
         "step SQLite statement: " + SqliteDetail(database_, result));
  }

  [[nodiscard]] std::string Text(int column) const {
    Require(sqlite3_column_type(statement_, column) == SQLITE_TEXT,
            "component_lifecycle_journal_corrupt",
            "lifecycle journal text column has an invalid type");
    const auto* bytes = sqlite3_column_text(statement_, column);
    const auto size = sqlite3_column_bytes(statement_, column);
    Require(bytes != nullptr && size >= 0, "component_lifecycle_journal_corrupt",
            "lifecycle journal text column is unreadable");
    return std::string(reinterpret_cast<const char*>(bytes), static_cast<std::size_t>(size));
  }

  [[nodiscard]] std::string Blob(int column) const {
    Require(sqlite3_column_type(statement_, column) == SQLITE_BLOB,
            "component_lifecycle_journal_corrupt",
            "lifecycle journal blob column has an invalid type");
    const auto* bytes = sqlite3_column_blob(statement_, column);
    const auto size = sqlite3_column_bytes(statement_, column);
    Require(size >= 0 && (size == 0 || bytes != nullptr), "component_lifecycle_journal_corrupt",
            "lifecycle journal blob column is unreadable");
    return std::string(static_cast<const char*>(bytes), static_cast<std::size_t>(size));
  }

  [[nodiscard]] std::int64_t Integer(int column) const {
    Require(sqlite3_column_type(statement_, column) == SQLITE_INTEGER,
            "component_lifecycle_journal_corrupt",
            "lifecycle journal integer column has an invalid type");
    return sqlite3_column_int64(statement_, column);
  }

  [[nodiscard]] bool IsNull(int column) const noexcept {
    return sqlite3_column_type(statement_, column) == SQLITE_NULL;
  }

 private:
  sqlite3* database_{};
  sqlite3_stmt* statement_{};
};

class Transaction final {
 public:
  explicit Transaction(sqlite3* database) : database_(database) {
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
  sqlite3* database_{};
  bool committed_{};
};

class ReadTransaction final {
 public:
  explicit ReadTransaction(sqlite3* database) : database_(database) { Exec(database_, "BEGIN"); }

  ~ReadTransaction() {
    if (!committed_) {
      sqlite3_exec(database_, "ROLLBACK", nullptr, nullptr, nullptr);
    }
  }

  void Commit() {
    Exec(database_, "COMMIT");
    committed_ = true;
  }

 private:
  sqlite3* database_{};
  bool committed_{};
};

[[nodiscard]] std::int64_t ScalarInteger(sqlite3* database, std::string_view sql) {
  Statement statement(database, sql);
  Require(statement.Step(), "component_lifecycle_journal_corrupt",
          "lifecycle journal scalar query returned no row");
  const auto value = statement.Integer(0);
  Require(!statement.Step(), "component_lifecycle_journal_corrupt",
          "lifecycle journal scalar query returned multiple rows");
  return value;
}

[[nodiscard]] std::filesystem::path ValidatePath(std::filesystem::path path) {
  Require(!path.empty() && path.filename() != "." && path.filename() != "..",
          "component_lifecycle_journal_path_invalid", "lifecycle journal database path is invalid");
  std::error_code error;
  path = std::filesystem::absolute(path, error).lexically_normal();
  Require(!error, "component_lifecycle_journal_path_invalid",
          "lifecycle journal path cannot be resolved");
  for (auto ancestor = path.parent_path(); !ancestor.empty(); ancestor = ancestor.parent_path()) {
    const auto status = std::filesystem::symlink_status(ancestor, error);
    Require(!error && std::filesystem::is_directory(status) && !std::filesystem::is_symlink(status),
            "component_lifecycle_journal_path_invalid",
            "lifecycle journal ancestors must be existing non-symlink directories");
    if (ancestor == ancestor.root_path()) {
      break;
    }
  }
  for (const auto& candidate : {path, std::filesystem::path(path.string() + "-wal"),
                                std::filesystem::path(path.string() + "-shm")}) {
    const auto status = std::filesystem::symlink_status(candidate, error);
    if (error == std::errc::no_such_file_or_directory) {
      error.clear();
      continue;
    }
    Require(!error && !std::filesystem::is_symlink(status),
            "component_lifecycle_journal_path_invalid",
            "lifecycle journal database or sidecar must not be a symlink");
    if (std::filesystem::exists(status)) {
      Require(std::filesystem::is_regular_file(status), "component_lifecycle_journal_path_invalid",
              "lifecycle journal database or sidecar must be a regular file");
    }
  }
  return path;
}

[[nodiscard]] std::set<std::string> SchemaObjectNames(sqlite3* database) {
  Statement statement(database,
                      "SELECT type || ':' || name FROM sqlite_schema "
                      "WHERE name NOT LIKE 'sqlite_%' ORDER BY type,name");
  std::set<std::string> result;
  while (statement.Step()) {
    result.insert(statement.Text(0));
  }
  return result;
}

using SchemaDefinition = std::tuple<std::string, std::string, std::string, std::string>;

[[nodiscard]] std::vector<SchemaDefinition> SchemaDefinitions(sqlite3* database) {
  Statement statement(database,
                      "SELECT type,name,tbl_name,sql FROM sqlite_schema "
                      "WHERE name NOT LIKE 'sqlite_%' ORDER BY type,name");
  std::vector<SchemaDefinition> result;
  while (statement.Step()) {
    result.emplace_back(statement.Text(0), statement.Text(1), statement.Text(2), statement.Text(3));
  }
  return result;
}

constexpr std::string_view kProgramNodeSchemaUpgrade =
    "ALTER TABLE cclc_lifecycle_cell_claims ADD COLUMN program_node_evidence BLOB;"
    "ALTER TABLE cclc_request_memberships ADD COLUMN rejection_blob BLOB;"
    "ALTER TABLE cclc_request_memberships ADD COLUMN rejection_sequence INTEGER";

constexpr std::string_view kRouteRecoverySchemaUpgrade =
    "ALTER TABLE cclc_lifecycle_cell_claims ADD COLUMN recovery_configuration BLOB";

constexpr std::string_view kNativeApplySchemaUpgrade = R"sql(
CREATE TABLE cclc_native_apply_adoptions (
  apply_id TEXT NOT NULL,
  apply_generation INTEGER NOT NULL,
  publication_generation INTEGER NOT NULL UNIQUE,
  public_pointer_digest TEXT NOT NULL,
  candidate_bundle_id TEXT NOT NULL,
  candidate_graph_revision TEXT NOT NULL,
  facts_digest TEXT NOT NULL,
  PRIMARY KEY(apply_id, apply_generation)
) STRICT;
CREATE TABLE cclc_native_apply_plans (
  singleton INTEGER PRIMARY KEY CHECK (singleton = 1),
  apply_id TEXT NOT NULL,
  apply_generation INTEGER NOT NULL CHECK (apply_generation > 0),
  candidate_bundle_id TEXT NOT NULL,
  candidate_graph_revision TEXT NOT NULL
) STRICT;
CREATE TABLE cclc_native_apply_plan_components (
  controllable_component_id TEXT PRIMARY KEY,
  component_lifecycle_state INTEGER NOT NULL CHECK (
    component_lifecycle_state BETWEEN 0 AND 7),
  component_lifecycle_generation INTEGER NOT NULL CHECK (
    component_lifecycle_generation > 0)
) STRICT;
CREATE TABLE cclc_native_apply_plan_routes (
  control_domain_id TEXT PRIMARY KEY,
  active_control_configuration_generation INTEGER NOT NULL CHECK (
    active_control_configuration_generation > 0)
) STRICT;
CREATE TABLE cclc_native_apply_reservations (
  reservation_kind TEXT NOT NULL CHECK (reservation_kind IN ('component', 'route')),
  identity TEXT NOT NULL,
  generation INTEGER NOT NULL CHECK (generation > 0),
  PRIMARY KEY(reservation_kind, identity)
) STRICT;
)sql";

// v12: a native Apply plan records which components/routes it retains
// unchanged and which predecessor components/domains it removes. The current
// v17 draft also seals the optional Q97 delegated acceptance receipts here;
// existing files are never upgraded in place.
constexpr std::string_view kNativeApplyDeltaSchemaUpgrade = R"sql(
ALTER TABLE cclc_native_apply_plan_components ADD COLUMN retained INTEGER NOT NULL DEFAULT 0
  CHECK (retained IN (0, 1));
ALTER TABLE cclc_native_apply_plan_routes ADD COLUMN retained INTEGER NOT NULL DEFAULT 0
  CHECK (retained IN (0, 1));
CREATE TABLE cclc_native_apply_plan_removals (
  removal_kind TEXT NOT NULL CHECK (removal_kind IN ('component', 'route')),
  identity TEXT NOT NULL,
  PRIMARY KEY(removal_kind, identity)
) STRICT;
ALTER TABLE cclc_native_apply_plans ADD COLUMN component_bring_up BLOB;
)sql";

// v14-v17: session-scoped requested lifecycle targets (with whether the
// component reached its target since startup or the last acknowledged
// incident), the last intervention-required cause per component domain, and
// component incidents with their bounded durable history events. A cancelled
// target keeps its row (present=0) so a re-added component never reuses a
// generation. At most one incident per component is open (acknowledged=0).
// An acknowledged incident names the operator's Recover by its ordinary
// request membership (owner epoch, request id), which retains its result.
// The per-component recovery generation advances in every commit that clears
// the driver's facts (the operator's Recover, a cancelled target); a driver
// write based on an older one is refused (no row reads as generation 0).
constexpr std::string_view kLifecycleTargetSchemaUpgrade = R"sql(
CREATE TABLE cclc_component_lifecycle_targets (
  controllable_component_id TEXT PRIMARY KEY,
  requested_state INTEGER NOT NULL CHECK (requested_state BETWEEN 0 AND 2),
  origin INTEGER NOT NULL CHECK (origin BETWEEN 0 AND 2),
  target_generation INTEGER NOT NULL CHECK (target_generation > 0),
  present INTEGER NOT NULL CHECK (present IN (0, 1)),
  reached INTEGER NOT NULL CHECK (reached IN (0, 1)),
  recovered_cause INTEGER CHECK (recovered_cause IS NULL OR recovered_cause BETWEEN 2 AND 10),
  retirement_reason INTEGER NOT NULL CHECK (retirement_reason BETWEEN 0 AND 2),
  CHECK ((present=1 AND retirement_reason=0) OR (present=0 AND retirement_reason IN (1,2)))
) STRICT;
CREATE TABLE cclc_component_intervention_causes (
  controllable_component_id TEXT NOT NULL,
  control_domain_id TEXT NOT NULL,
  cause_class INTEGER NOT NULL CHECK (cause_class BETWEEN 2 AND 10),
  observed_at_ns INTEGER NOT NULL CHECK (observed_at_ns >= 0),
  PRIMARY KEY(controllable_component_id, control_domain_id)
) STRICT;
CREATE TABLE cclc_component_incidents (
  incident_id TEXT PRIMARY KEY,
  incident_sequence INTEGER NOT NULL UNIQUE CHECK (incident_sequence > 0),
  target_kind TEXT NOT NULL DEFAULT 'component'
    CHECK (target_kind IN ('component','cell_observer')),
  controllable_component_id TEXT,
  observer_cell_id TEXT,
  observer_latest_exit_id TEXT,
  observer_latest_exit_digest TEXT,
  cause_class INTEGER NOT NULL CHECK (cause_class BETWEEN 2 AND 10),
  vendor_codes_json TEXT NOT NULL,
  detected_at_ns INTEGER NOT NULL CHECK (detected_at_ns >= 0),
  summary TEXT NOT NULL,
  containment_state INTEGER CHECK (containment_state IS NULL OR containment_state BETWEEN 0 AND 2),
  record_json TEXT NOT NULL,
  acknowledged INTEGER NOT NULL CHECK (acknowledged IN (0, 1)),
  acknowledged_by TEXT,
  acknowledged_at_ns INTEGER CHECK (acknowledged_at_ns IS NULL OR acknowledged_at_ns >= 0),
  acknowledgement_request_id TEXT,
  acknowledgement_owner_epoch TEXT,
  CHECK ((target_kind='component' AND controllable_component_id IS NOT NULL AND
          containment_state IS NOT NULL AND observer_cell_id IS NULL AND
          observer_latest_exit_id IS NULL AND observer_latest_exit_digest IS NULL) OR
         (target_kind='cell_observer' AND controllable_component_id IS NULL AND
          containment_state IS NULL AND observer_cell_id IS NOT NULL AND
          length(CAST(observer_cell_id AS BLOB)) BETWEEN 1 AND 256 AND
          observer_latest_exit_id IS NOT NULL AND
          length(CAST(observer_latest_exit_id AS BLOB)) BETWEEN 1 AND 128 AND
          observer_latest_exit_digest IS NOT NULL AND length(observer_latest_exit_digest)=64 AND
          observer_latest_exit_digest NOT GLOB '*[^0-9a-f]*')),
  CHECK ((acknowledged = 0 AND acknowledged_by IS NULL AND acknowledged_at_ns IS NULL AND
          acknowledgement_request_id IS NULL AND acknowledgement_owner_epoch IS NULL) OR
         (acknowledged = 1 AND acknowledged_by IS NOT NULL AND acknowledged_at_ns IS NOT NULL AND
          acknowledgement_request_id IS NOT NULL AND acknowledgement_owner_epoch IS NOT NULL))
) STRICT;
CREATE UNIQUE INDEX one_open_cclc_component_incident
  ON cclc_component_incidents(controllable_component_id)
  WHERE target_kind='component' AND acknowledged=0;
CREATE UNIQUE INDEX one_open_cclc_cell_observer_incident
  ON cclc_component_incidents(observer_cell_id)
  WHERE target_kind='cell_observer' AND acknowledged=0;
CREATE UNIQUE INDEX one_cclc_component_incident_recovery_request
  ON cclc_component_incidents(acknowledgement_owner_epoch, acknowledgement_request_id)
  WHERE target_kind='component' AND acknowledged=1;
CREATE TABLE cclc_component_incident_events (
  event_sequence INTEGER PRIMARY KEY CHECK (event_sequence > 0),
  incident_id TEXT NOT NULL,
  event_kind TEXT NOT NULL CHECK (event_kind IN
    ('component_incident', 'component_incident_acknowledged')),
  occurred_at_ns INTEGER NOT NULL CHECK (occurred_at_ns >= 0),
  payload_json TEXT NOT NULL
) STRICT;
CREATE TABLE cclc_component_recovery_generations (
  controllable_component_id TEXT PRIMARY KEY,
  recovery_generation INTEGER NOT NULL CHECK (recovery_generation > 0)
) STRICT;
)sql";

[[nodiscard]] bool ValidCCLCClaimIntent(const CellAdmissionClaimIntentDTO& intent) {
  return ValidateCellAdmissionClaimIntent(intent).ok() &&
         intent.owner_role == CellAdmissionOwnerRoleDTO::kComponentLifecycleOwner &&
         (intent.claim_kind == CellAdmissionClaimKindDTO::kInterventionOrRecovery ||
          (intent.claim_kind == CellAdmissionClaimKindDTO::kComponentLifecycle &&
           intent.scope.resources.empty() && intent.scope.configurations.empty()));
}

[[nodiscard]] std::optional<std::string> RecoveryConfigurationBytes(
    const CellAdmissionClaimIntentDTO& intent,
    const std::optional<ContentDigest>& original_request_digest = std::nullopt) {
  Require(ValidCCLCClaimIntent(intent), "component_lifecycle_claim_invalid",
          "CCLC claim does not carry an exact supported owner intent");
  const bool selected = intent.owner_transaction_id.starts_with("selected-lifecycle.");
  Require(selected == original_request_digest.has_value() &&
              (!original_request_digest ||
               IsExactLowercaseSha256Digest(original_request_digest->value())),
          "component_lifecycle_claim_invalid",
          "selected claim requires its original request digest");
  if (intent.claim_kind != CellAdmissionClaimKindDTO::kInterventionOrRecovery) return std::nullopt;
  if (selected)
    return detail::EncodeCCLCSelectedBringUpClaimIntent({intent, *original_request_digest});
  return detail::EncodeCCLCRouteRecoveryConfiguration(intent.scope.configurations.front());
}

std::optional<ContentDigest> RestoreRouteRecoveryClaim(CellAdmissionClaimIntentDTO& intent,
                                                       std::string_view bytes) {
  if (detail::IsCCLCSelectedBringUpClaimIntentEncoding(bytes)) {
    const auto record = detail::DecodeCCLCSelectedBringUpClaimIntent(bytes);
    const auto& restored = record.full_claim_intent;
    // The blob supplies scope/evidence, never overrides the SQL owner identity.
    Require(
        restored.owner_role == intent.owner_role && restored.owner_epoch == intent.owner_epoch &&
            restored.owner_transaction_id == intent.owner_transaction_id &&
            restored.request_id == intent.request_id &&
            restored.request_payload_digest == intent.request_payload_digest &&
            restored.expected_admission_generation == intent.expected_admission_generation &&
            restored.program_node_evidence == intent.program_node_evidence &&
            restored.program_cycle_evidence == intent.program_cycle_evidence,
        "component_lifecycle_journal_corrupt", "selected claim blob crosses its stored identity");
    intent = restored;
    return record.original_request_payload_digest;
  }
  Require(!intent.owner_transaction_id.starts_with("selected-lifecycle."),
          "component_lifecycle_journal_corrupt", "selected claim has no selected intent encoding");
  intent.claim_kind = CellAdmissionClaimKindDTO::kInterventionOrRecovery;
  intent.scope.configurations = {detail::DecodeCCLCRouteRecoveryConfiguration(bytes)};
  intent.intervention_evidence = CellAdmissionInterventionEvidenceDTO{
      intent.owner_role, intent.owner_epoch, intent.owner_transaction_id,
      intent.request_payload_digest, std::nullopt};
  return std::nullopt;
}

// Validate a stored attempt against the complete acquired/attempted intent.
// Selected caller identity stays in this existing BLOB, separately from offer proof.
std::optional<ContentDigest> OriginalRequestDigestFromRecovery(
    const CellAdmissionClaimIntentDTO& intent, const std::optional<std::string>& bytes) {
  if (intent.owner_transaction_id.starts_with("selected-lifecycle.")) {
    Require(bytes && detail::IsCCLCSelectedBringUpClaimIntentEncoding(*bytes),
            "component_lifecycle_journal_corrupt", "selected claim intent encoding is missing");
    const auto record = detail::DecodeCCLCSelectedBringUpClaimIntent(*bytes);
    Require(record.full_claim_intent == intent, "component_lifecycle_journal_corrupt",
            "selected claim blob differs from the exact full intent");
    return record.original_request_payload_digest;
  }
  Require(bytes == RecoveryConfigurationBytes(intent), "component_lifecycle_journal_corrupt",
          "claim recovery configuration differs from its intent");
  return std::nullopt;
}

// Terminal owner work is still active evidence while its Kernel claim is
// unreleased. Only settled results (and no-admission rejections) are history.
constexpr std::string_view kSettledResults = R"sql(
SELECT 'component_lifecycle' AS request_kind,t.transaction_sequence,t.membership_sequence,
       t.terminal_sequence,length(t.status_blob)+coalesce((SELECT sum(
         length(a.prepared_attempt_blob)+coalesce(length(a.completed_attempt_blob),0))
         FROM cclc_effect_attempts a WHERE a.lifecycle_transaction_id=t.lifecycle_transaction_id),0)
       AS retained_bytes
FROM lifecycle_transactions t WHERE t.terminal=1 AND NOT EXISTS (
  SELECT 1 FROM cclc_lifecycle_cell_claims c WHERE c.lifecycle_transaction_id=t.lifecycle_transaction_id
  AND c.release_receipt_recorded=0)
UNION ALL
SELECT 'route_selection',t.transaction_sequence,t.membership_sequence,t.terminal_sequence,
       length(t.status_blob)+coalesce((SELECT sum(length(a.prepared_attempt_blob)+
         coalesce(length(a.completed_attempt_blob),0)) FROM cclc_route_effect_attempts a
         WHERE a.route_selection_transaction_id=t.route_selection_transaction_id),0)
FROM route_selection_transactions t WHERE t.terminal=1 AND NOT EXISTS (
  SELECT 1 FROM cclc_lifecycle_cell_claims c WHERE c.lifecycle_transaction_id=t.route_selection_transaction_id
  AND c.release_receipt_recorded=0)
UNION ALL
SELECT 'rejection',membership_sequence,membership_sequence,rejection_sequence,length(rejection_blob)
FROM cclc_request_memberships m WHERE retained_result=1 AND rejection_blob IS NOT NULL
AND NOT EXISTS (SELECT 1 FROM cclc_lifecycle_cell_claims c
                WHERE c.owner_epoch=m.owner_epoch AND c.request_id=m.request_id
                AND c.release_receipt_recorded=0)
UNION ALL
SELECT 'lifecycle_target_retirement',membership_sequence,membership_sequence,
       target_retirement_sequence,length(target_retirement_blob)
FROM cclc_request_memberships
WHERE retained_result=1 AND target_retirement_blob IS NOT NULL
)sql";

[[nodiscard]] const std::vector<SchemaDefinition>& ExpectedSchemaDefinitions() {
  const auto build = [] {
    sqlite3* expected = nullptr;
    const auto open_result = sqlite3_open_v2(
        ":memory:", &expected, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
        nullptr);
    if (open_result != SQLITE_OK) {
      const auto detail = SqliteDetail(expected, open_result);
      if (expected != nullptr) {
        sqlite3_close(expected);
      }
      Fail("component_lifecycle_journal_storage_unsupported",
           "create expected lifecycle journal schema: " + detail);
    }
    try {
      Exec(expected, kSchema);
      Exec(expected, kProgramNodeSchemaUpgrade);
      Exec(expected, kRouteRecoverySchemaUpgrade);
      Exec(expected, kNativeApplySchemaUpgrade);
      Exec(expected, kNativeApplyDeltaSchemaUpgrade);
      Exec(expected, kLifecycleTargetSchemaUpgrade);
      auto result = SchemaDefinitions(expected);
      const auto close_result = sqlite3_close(expected);
      if (close_result != SQLITE_OK) {
        Fail("component_lifecycle_journal_storage_failure",
             "close expected lifecycle journal schema: " +
                 std::string(sqlite3_errstr(close_result)));
      }
      expected = nullptr;
      return result;
    } catch (...) {
      if (expected != nullptr) {
        sqlite3_close(expected);
      }
      throw;
    }
  };
  static const auto current = build();
  return current;
}

[[nodiscard]] const std::set<std::string>& ExpectedSchemaObjects() {
  static const std::set<std::string> names{
      "index:lifecycle_transaction_terminal_order",
      "index:route_selection_transaction_terminal_order",
      "index:cclc_effect_attempt_transaction_order",
      "index:cclc_route_effect_attempt_transaction_order",
      "index:one_active_cclc_owner_epoch",
      "table:cclc_journal_metadata",
      "table:cclc_owner_epochs",
      "table:cclc_request_memberships",
      "table:cclc_lifecycle_cell_claims",
      "table:cclc_lifecycle_transaction_identity_guards",
      "table:cclc_route_transaction_identity_guards",
      "table:lifecycle_component_watermarks",
      "table:lifecycle_component_current_facts",
      "table:control_domain_active_configuration_watermarks",
      "table:control_domain_active_configurations",
      "table:cclc_effect_attempts",
      "table:cclc_route_effect_attempts",
      "table:cclc_native_apply_adoptions",
      "table:cclc_native_apply_plan_components",
      "table:cclc_native_apply_plan_removals",
      "table:cclc_native_apply_plan_routes",
      "table:cclc_native_apply_plans",
      "table:cclc_native_apply_reservations",
      "table:cclc_component_lifecycle_targets",
      "table:cclc_component_intervention_causes",
      "table:cclc_component_incidents",
      "table:cclc_component_incident_events",
      "table:cclc_component_recovery_generations",
      "index:one_open_cclc_component_incident",
      "index:one_open_cclc_cell_observer_incident",
      "index:one_cclc_component_incident_recovery_request",
      "table:lifecycle_transactions",
      "table:route_selection_transactions",
  };
  return names;
}

[[nodiscard]] std::uint64_t Nonnegative(std::int64_t value, std::string_view label) {
  Require(value >= 0, "component_lifecycle_journal_corrupt", std::string(label) + " is negative");
  return static_cast<std::uint64_t>(value);
}

[[nodiscard]] std::uint64_t Positive(std::int64_t value, std::string_view label) {
  Require(value > 0, "component_lifecycle_journal_corrupt",
          std::string(label) + " is not positive");
  return static_cast<std::uint64_t>(value);
}

[[nodiscard]] bool FitsSqliteInteger(std::uint64_t value) noexcept {
  return value <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
}

[[nodiscard]] bool Terminal(const ComponentLifecycleStatusDTO& status) {
  return status.terminal_result.has_value();
}

[[nodiscard]] bool ImmutableAcceptanceEqual(const ComponentLifecycleStatusDTO& left,
                                            const ComponentLifecycleStatusDTO& right) {
  return left.acceptance == right.acceptance && left.accepted_intent == right.accepted_intent &&
         left.accepted_at_ns == right.accepted_at_ns &&
         left.selected_bring_up == right.selected_bring_up && left.apply_bring_up == right.apply_bring_up;
}

[[nodiscard]] bool RequestedStateMonotonic(const ComponentLifecycleStatusDTO& previous,
                                           const ComponentLifecycleStatusDTO& next) {
  if (previous.requested_state == next.requested_state) {
    return true;
  }
  return previous.accepted_intent.intent == ComponentLifecycleIntentKindDTO::kRecover &&
         previous.requested_state.presence ==
             ComponentLifecycleRequestedStatePresenceDTO::kUnresolved &&
         next.requested_state.presence == ComponentLifecycleRequestedStatePresenceDTO::kResolved;
}

[[nodiscard]] std::string_view EffectKindName(CCLCEffectKindDTO kind) noexcept {
  switch (kind) {
    case CCLCEffectKindDTO::kBrandLifecyclePrimitive:
      return "brand_lifecycle_primitive";
  }
  return {};
}

[[nodiscard]] std::string EffectIdentity(const CCLCEffectAttemptDTO& attempt) {
  const auto prefix = attempt.lifecycle_transaction_id.value() + "\x1f";
  switch (attempt.effect_kind) {
    case CCLCEffectKindDTO::kBrandLifecyclePrimitive:
      return prefix +
             "brand:" + attempt.lifecycle_primitive_intent->lifecycle_primitive_attempt_id.value();
  }
  return {};
}

[[nodiscard]] std::string BrandEffectIdentity(const LifecycleTransactionId& transaction_id,
                                              const LifecyclePrimitiveAttemptId& attempt_id) {
  return transaction_id.value() +
         "\x1f"
         "brand:" +
         attempt_id.value();
}

[[nodiscard]] CCLCEffectAttemptDTO PreparedForm(CCLCEffectAttemptDTO completed) {
  completed.state = CCLCEffectAttemptStateDTO::kPrepared;
  completed.lifecycle_primitive_result.reset();
  return completed;
}

[[nodiscard]] bool Terminal(const RouteSelectionStatusDTO& status) noexcept {
  return status.terminal_result.has_value();
}

[[nodiscard]] std::string_view RouteEffectKindName(CCLCRouteEffectKindDTO kind) noexcept {
  switch (kind) {
    case CCLCRouteEffectKindDTO::kKernelRouteConfigurationFenceAcquire:
      return "kernel_route_configuration_fence_acquire";
    case CCLCRouteEffectKindDTO::kControllerLifecycleTransition:
      return "controller_lifecycle_transition";
    case CCLCRouteEffectKindDTO::kKernelRouteConfigurationFenceRelease:
      return "kernel_route_configuration_fence_release";
  }
  return {};
}

[[nodiscard]] std::string RouteEffectIdentity(const CCLCRouteEffectAttemptDTO& attempt) {
  return attempt.route_selection_transaction_id.value() +
         "\x1froute:" + std::to_string(attempt.effect_sequence);
}

[[nodiscard]] CCLCRouteEffectAttemptDTO PreparedForm(CCLCRouteEffectAttemptDTO completed) {
  completed.state = CCLCRouteEffectAttemptStateDTO::kPrepared;
  completed.result.reset();
  return completed;
}

[[nodiscard]] bool RouteEffectMonotonic(RouteSelectionEffectDTO previous,
                                        RouteSelectionEffectDTO next) noexcept {
  switch (previous) {
    case RouteSelectionEffectDTO::kNone:
      return true;
    case RouteSelectionEffectDTO::kKnownNoEffect:
      return next != RouteSelectionEffectDTO::kNone;
    case RouteSelectionEffectDTO::kEffectObserved:
    case RouteSelectionEffectDTO::kPartialEffectObserved:
      return next == RouteSelectionEffectDTO::kEffectObserved ||
             next == RouteSelectionEffectDTO::kPartialEffectObserved ||
             next == RouteSelectionEffectDTO::kEffectUncertain;
    case RouteSelectionEffectDTO::kEffectUncertain:
      return next != RouteSelectionEffectDTO::kNone;
  }
  return false;
}

[[nodiscard]] bool RouteObservationMonotonic(
    const std::optional<ControllerLifecycleObservationDTO>& previous,
    const std::optional<ControllerLifecycleObservationDTO>& next) noexcept {
  if (!previous.has_value()) {
    return true;
  }
  if (!next.has_value() || previous->query != next->query ||
      next->source_sequence < previous->source_sequence) {
    return false;
  }
  return next->source_sequence != previous->source_sequence || *next == *previous;
}

[[nodiscard]] bool RouteReconciliationMonotonic(
    const std::optional<ControllerLifecycleReconciliationDTO>& previous,
    const std::optional<ControllerLifecycleReconciliationDTO>& next) noexcept {
  if (!previous.has_value()) {
    return true;
  }
  if (!next.has_value() || previous->query != next->query ||
      next->source_sequence < previous->source_sequence) {
    return false;
  }
  return next->source_sequence != previous->source_sequence || *next == *previous;
}

[[nodiscard]] bool RouteStatusReplacementIsMonotonic(const RouteSelectionStatusDTO& previous,
                                                     const RouteSelectionStatusDTO& next,
                                                     bool has_unresolved_prepared_effect) noexcept {
  if (Terminal(previous) || previous.acceptance != next.acceptance ||
      previous.accepted_intent != next.accepted_intent ||
      previous.accepted_at_ns != next.accepted_at_ns ||
      !std::includes(previous.affected_control_binding_ids.begin(),
                     previous.affected_control_binding_ids.end(),
                     next.affected_control_binding_ids.begin(),
                     next.affected_control_binding_ids.end()) ||
      !std::includes(previous.affected_controller_role_ids.begin(),
                     previous.affected_controller_role_ids.end(),
                     next.affected_controller_role_ids.begin(),
                     next.affected_controller_role_ids.end()) ||
      next.status_sequence <= previous.status_sequence ||
      !RouteEffectMonotonic(previous.effect, next.effect) ||
      !RouteObservationMonotonic(previous.controller_observation, next.controller_observation) ||
      !RouteReconciliationMonotonic(previous.controller_reconciliation,
                                    next.controller_reconciliation) ||
      previous.controller_transition_results.size() > next.controller_transition_results.size() ||
      !std::equal(previous.controller_transition_results.begin(),
                  previous.controller_transition_results.end(),
                  next.controller_transition_results.begin()) ||
      (previous.accepted_fence.has_value() && previous.accepted_fence != next.accepted_fence) ||
      (previous.released_fence.has_value() && previous.released_fence != next.released_fence) ||
      (previous.resulting_active_control_configuration.has_value() &&
       previous.resulting_active_control_configuration !=
           next.resulting_active_control_configuration)) {
    return false;
  }
  const bool owner_changed = previous.status_owner_epoch != next.status_owner_epoch;
  if (!owner_changed) {
    return next.latest_updated_at_ns >= previous.latest_updated_at_ns &&
           next.remaining_duration_ns <= previous.remaining_duration_ns;
  }
  const bool reconciliation = next.reconciliation_required &&
                              (next.phase == RouteSelectionTransactionPhaseDTO::kOutcomeUnknown ||
                               next.phase == RouteSelectionTransactionPhaseDTO::kReconciling);
  const bool known_no_effect_terminal =
      !has_unresolved_prepared_effect &&
      next.terminal_result ==
          std::optional(RouteSelectionTerminalResultDTO::kFailedKnownNoEffect) &&
      !next.accepted_fence.has_value();
  return reconciliation || known_no_effect_terminal;
}

[[nodiscard]] bool AttemptsRetained(const ComponentLifecycleStatusDTO& previous,
                                    const ComponentLifecycleStatusDTO& next) {
  if (previous.domains.size() != next.domains.size()) {
    return false;
  }
  for (std::size_t index = 0U; index < previous.domains.size(); ++index) {
    const auto& prior = previous.domains[index];
    const auto& current = next.domains[index];
    if (prior.control_domain_id != current.control_domain_id ||
        prior.expected_domain != current.expected_domain ||
        prior.current_scope != current.current_scope ||
        prior.primitive_attempts.size() > current.primitive_attempts.size() ||
        !std::equal(prior.primitive_attempts.begin(), prior.primitive_attempts.end(),
                    current.primitive_attempts.begin())) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] bool ReconciliationEvidenceMonotonic(
    const std::optional<LifecycleReconciliationEvidenceDTO>& previous,
    const std::optional<LifecycleReconciliationEvidenceDTO>& next) {
  if (!previous.has_value()) {
    return true;
  }
  if (!next.has_value()) {
    return false;
  }
  const bool same_source =
      previous->provider_instance_id == next->provider_instance_id &&
      previous->provider_manifest_id == next->provider_manifest_id &&
      previous->provider_manifest_revision == next->provider_manifest_revision &&
      previous->provider_runtime_unit_id == next->provider_runtime_unit_id &&
      previous->provider_runtime_epoch == next->provider_runtime_epoch;
  if (!same_source || previous->source_sequence > next->source_sequence ||
      previous->observation_generation > next->observation_generation ||
      previous->observed_at_ns > next->observed_at_ns) {
    return false;
  }
  if (previous->observation_generation == next->observation_generation &&
      (previous->observed_state != next->observed_state ||
       previous->postcondition_result != next->postcondition_result)) {
    return false;
  }
  if (previous->source_sequence == next->source_sequence &&
      previous->observation_generation == next->observation_generation) {
    return *previous == *next;
  }
  return previous->observation_generation == next->observation_generation ||
         previous->source_sequence < next->source_sequence;
}

[[nodiscard]] bool ReconciliationEvidenceIsFresh(
    const std::optional<LifecycleReconciliationEvidenceDTO>& previous,
    const std::optional<LifecycleReconciliationEvidenceDTO>& next) {
  if (!next.has_value()) {
    return false;
  }
  return !previous.has_value() ||
         (previous->provider_instance_id == next->provider_instance_id &&
          previous->provider_manifest_id == next->provider_manifest_id &&
          previous->provider_manifest_revision == next->provider_manifest_revision &&
          previous->provider_runtime_unit_id == next->provider_runtime_unit_id &&
          previous->provider_runtime_epoch == next->provider_runtime_epoch &&
          previous->source_sequence < next->source_sequence &&
          previous->observation_generation < next->observation_generation);
}

[[nodiscard]] bool DomainEffectMonotonic(const ComponentLifecycleDomainStatusDTO& previous,
                                         const ComponentLifecycleDomainStatusDTO& next) {
  using Effect = ComponentLifecycleDomainEffectDTO;
  if (!ReconciliationEvidenceMonotonic(previous.reconciliation_evidence,
                                       next.reconciliation_evidence)) {
    return false;
  }
  switch (previous.effect) {
    case Effect::kNone:
      return true;
    case Effect::kKnownNoEffect:
      return next.effect != Effect::kNone;
    case Effect::kEffectObserved:
    case Effect::kPartialEffectObserved:
      return next.effect == Effect::kEffectObserved ||
             next.effect == Effect::kPartialEffectObserved ||
             (next.effect == Effect::kEffectUncertain &&
              next.primitive_attempts.size() == previous.primitive_attempts.size() + 1U &&
              next.primitive_attempts.back().dispatch_state ==
                  LifecyclePrimitiveDispatchStateDTO::kDispatchedUncertain);
    case Effect::kEffectUncertain:
      if (next.effect == Effect::kEffectUncertain) {
        return true;
      }
      if (next.effect == Effect::kNone) {
        return false;
      }
      return ReconciliationEvidenceIsFresh(previous.reconciliation_evidence,
                                           next.reconciliation_evidence);
  }
  return false;
}

[[nodiscard]] bool DomainActionMonotonic(const ComponentLifecycleDomainStatusDTO& previous,
                                         const ComponentLifecycleDomainStatusDTO& next) {
  using Action = ComponentLifecycleDomainActionStateDTO;
  if (previous.action_state == Action::kSucceeded || previous.action_state == Action::kFailed) {
    return next.action_state == previous.action_state;
  }
  if (previous.action_state == Action::kOutcomeUnknown ||
      previous.action_state == Action::kReconciling) {
    const bool resolved_by_fresh_evidence = ReconciliationEvidenceIsFresh(
        previous.reconciliation_evidence, next.reconciliation_evidence);
    return next.action_state == Action::kOutcomeUnknown ||
           next.action_state == Action::kReconciling || next.action_state == Action::kSucceeded ||
           next.action_state == Action::kFailed ||
           (resolved_by_fresh_evidence &&
            (next.action_state == Action::kExecuting || next.action_state == Action::kVerifying));
  }
  return true;
}

[[nodiscard]] bool DomainEvidenceMonotonic(const ComponentLifecycleStatusDTO& previous,
                                           const ComponentLifecycleStatusDTO& next) {
  if (previous.domains.size() != next.domains.size()) {
    return false;
  }
  for (std::size_t index = 0U; index < previous.domains.size(); ++index) {
    const auto& prior = previous.domains[index];
    const auto& current = next.domains[index];
    if (prior.control_domain_id != current.control_domain_id ||
        prior.resolved_provider_occurrence != current.resolved_provider_occurrence ||
        !DomainEffectMonotonic(prior, current) || !DomainActionMonotonic(prior, current)) {
      return false;
    }
  }
  return true;
}

// The original delegated runtime records actual installation once, before any
// lifecycle primitive. Recovered owners cannot reconstruct this transition.
[[nodiscard]] bool ApplyInstallationEvidenceIsMonotonic(
    const ComponentLifecycleStatusDTO& previous, const ComponentLifecycleStatusDTO& next) {
  using Phase = ComponentLifecycleTransactionPhaseDTO;
  using Action = ComponentLifecycleDomainActionStateDTO;
  using Effect = ComponentLifecycleDomainEffectDTO;
  if (!previous.apply_bring_up || previous.selected_bring_up ||
      previous.phase != Phase::kAccepted || next.phase != Phase::kVerifying ||
      previous.status_owner_epoch != previous.accepted_intent.request_context.request_owner_epoch ||
      next.status_owner_epoch != previous.status_owner_epoch ||
      previous.domains.size() != next.domains.size() ||
      previous.previous_state != next.previous_state ||
      previous.error || next.error || previous.reconciliation_required || next.reconciliation_required)
    return false;
  for (std::size_t index = 0U; index < previous.domains.size(); ++index) {
    const auto& prior = previous.domains[index];
    const auto& current = next.domains[index];
    if (prior.control_domain_id != current.control_domain_id ||
        prior.expected_domain != current.expected_domain ||
        prior.requested_state != current.requested_state ||
        !prior.primitive_attempts.empty() || !current.primitive_attempts.empty() ||
        prior.reconciliation_evidence || current.reconciliation_evidence ||
        prior.effect != Effect::kNone ||
        (prior.action_state != Action::kReserved && prior.action_state != Action::kNotStarted) ||
        (current.effect != Effect::kNone && current.effect != Effect::kKnownNoEffect)) return false;
    if (!current.expected_domain) {
      if (current.current_scope != prior.current_scope ||
          current.resolved_provider_occurrence != prior.resolved_provider_occurrence ||
          current.previous_state != prior.previous_state || current.current_state != prior.current_state ||
          current.action_state != Action::kReserved || current.effect != Effect::kKnownNoEffect)
        return false;
      continue;
    }
    const auto& expected = *current.expected_domain;
    if (!current.current_scope || !current.resolved_provider_occurrence ||
        (current.action_state != Action::kReserved && current.action_state != Action::kSucceeded) ||
        current.current_scope->control_domain_revision != expected.control_domain_revision ||
        current.current_scope->controller_topology_id != expected.controller_topology_id ||
        current.current_scope->topology_revision != expected.topology_revision ||
        current.current_scope->current_control_bindings != expected.expected_control_bindings ||
        (prior.previous_state ? current.previous_state != prior.previous_state
                              : current.previous_state != current.current_state)) return false;
  }
  return true;
}

[[nodiscard]] bool TerminalStatusIsQuiescent(const ComponentLifecycleStatusDTO& status) {
  return Terminal(status) && !status.reconciliation_required;
}

[[nodiscard]] bool ReplacementTerminalIsSupportedByRetainedEvidence(
    const ComponentLifecycleStatusDTO& previous, const ComponentLifecycleStatusDTO& next) {
  if (!TerminalStatusIsQuiescent(next) || previous.previous_state != next.previous_state ||
      previous.current_state != next.current_state ||
      previous.domains.size() != next.domains.size() || !next.terminal_result.has_value()) {
    return false;
  }

  bool retained_observed_effect = false;
  for (std::size_t index = 0U; index < previous.domains.size(); ++index) {
    const auto& prior = previous.domains[index];
    const auto& current = next.domains[index];
    if (prior.control_domain_id != current.control_domain_id ||
        prior.expected_domain != current.expected_domain ||
        prior.current_scope != current.current_scope ||
        prior.resolved_provider_occurrence != current.resolved_provider_occurrence ||
        prior.previous_state != current.previous_state ||
        prior.current_state != current.current_state ||
        prior.requested_state != current.requested_state ||
        prior.primitive_attempts != current.primitive_attempts ||
        prior.reconciliation_evidence != current.reconciliation_evidence) {
      return false;
    }

    switch (*next.terminal_result) {
      case ComponentLifecycleTerminalResultDTO::kSucceeded:
        if (prior.action_state != ComponentLifecycleDomainActionStateDTO::kSucceeded ||
            current.action_state != prior.action_state || current.effect != prior.effect) {
          return false;
        }
        break;
      case ComponentLifecycleTerminalResultDTO::kFailedKnownNoEffect:
        if ((prior.effect != ComponentLifecycleDomainEffectDTO::kNone &&
             prior.effect != ComponentLifecycleDomainEffectDTO::kKnownNoEffect) ||
            (current.effect != ComponentLifecycleDomainEffectDTO::kNone &&
             current.effect != ComponentLifecycleDomainEffectDTO::kKnownNoEffect)) {
          return false;
        }
        break;
      case ComponentLifecycleTerminalResultDTO::kFailedPartialEffect:
        if (prior.effect == ComponentLifecycleDomainEffectDTO::kEffectObserved ||
            prior.effect == ComponentLifecycleDomainEffectDTO::kPartialEffectObserved) {
          retained_observed_effect = true;
          if (current.effect != prior.effect) {
            return false;
          }
        } else if (prior.effect == ComponentLifecycleDomainEffectDTO::kKnownNoEffect) {
          if (current.effect != ComponentLifecycleDomainEffectDTO::kKnownNoEffect) {
            return false;
          }
        } else if (prior.effect == ComponentLifecycleDomainEffectDTO::kNone) {
          if (current.effect != ComponentLifecycleDomainEffectDTO::kNone &&
              current.effect != ComponentLifecycleDomainEffectDTO::kKnownNoEffect) {
            return false;
          }
        } else {
          return false;
        }
        break;
    }
  }
  return *next.terminal_result != ComponentLifecycleTerminalResultDTO::kFailedPartialEffect ||
         retained_observed_effect;
}

[[nodiscard]] bool StatusEvidenceIsMonotonic(const ComponentLifecycleStatusDTO& previous,
                                             const ComponentLifecycleStatusDTO& next) {
  const bool owner_changed = previous.status_owner_epoch != next.status_owner_epoch;
  const bool ordinary_closure_retained = previous.previous_state == next.previous_state &&
                                         AttemptsRetained(previous, next) &&
                                         DomainEvidenceMonotonic(previous, next);
  return !Terminal(previous) && ImmutableAcceptanceEqual(previous, next) &&
         RequestedStateMonotonic(previous, next) &&
         next.status_sequence > previous.status_sequence &&
         (ordinary_closure_retained || ApplyInstallationEvidenceIsMonotonic(previous, next)) &&
         (owner_changed || next.latest_updated_at_ns >= previous.latest_updated_at_ns);
}

[[nodiscard]] bool StatusReplacementIsMonotonic(const ComponentLifecycleStatusDTO& previous,
                                                const ComponentLifecycleStatusDTO& next) {
  const bool owner_changed = previous.status_owner_epoch != next.status_owner_epoch;
  return StatusEvidenceIsMonotonic(previous, next) &&
         (!owner_changed ||
          (next.reconciliation_required &&
           (next.phase == ComponentLifecycleTransactionPhaseDTO::kOutcomeUnknown ||
            next.phase == ComponentLifecycleTransactionPhaseDTO::kReconciling)));
}

[[nodiscard]] bool OwnerReplacementStatusIsMonotonic(const ComponentLifecycleStatusDTO& previous,
                                                     const ComponentLifecycleStatusDTO& next,
                                                     bool has_unresolved_prepared_effect) {
  const bool owner_changed = previous.status_owner_epoch != next.status_owner_epoch;
  const bool reconciliation_status =
      next.reconciliation_required &&
      (next.phase == ComponentLifecycleTransactionPhaseDTO::kOutcomeUnknown ||
       next.phase == ComponentLifecycleTransactionPhaseDTO::kReconciling);
  const bool replacement_transition_valid =
      !owner_changed || reconciliation_status ||
      (!has_unresolved_prepared_effect &&
       ReplacementTerminalIsSupportedByRetainedEvidence(previous, next));
  return StatusEvidenceIsMonotonic(previous, next) && replacement_transition_valid;
}

}  // namespace

class SqliteCCLCJournal::Impl final {
 public:
  Impl(std::filesystem::path database_path, CellId cell_id,
       std::size_t exact_result_history_capacity, std::size_t terminal_result_bytes_capacity,
       std::function<void(const ComponentIncidentPruneNotice&)> on_incidents_pruned,
       NativeApplyParentPreparationFailureSource preparation_failure_source)
      : database_path_(ValidatePath(std::move(database_path))),
        cell_id_(std::move(cell_id)),
        exact_result_history_capacity_(exact_result_history_capacity),
        terminal_result_bytes_capacity_(terminal_result_bytes_capacity),
        on_incidents_pruned_(std::move(on_incidents_pruned)),
        preparation_failure_source_(std::move(preparation_failure_source)) {
    Require(!cell_id_.empty() && exact_result_history_capacity_ > 0U &&
                terminal_result_bytes_capacity_ > 0U &&
                cell_id_.value().size() <= kMaxLifecycleIdentityBytes &&
                exact_result_history_capacity_ <= kMaximumCCLCExactResultHistory &&
                terminal_result_bytes_capacity_ <= kMaximumCCLCTerminalResultBytes,
            "component_lifecycle_journal_configuration_invalid",
            "lifecycle journal cell identity or exact-result bound is invalid");
    Require(sqlite3_libversion_number() >= 3037000,
            "component_lifecycle_journal_storage_unsupported",
            "lifecycle journal requires SQLite 3.37.0 or newer");
    const auto result = sqlite3_open_v2(
        database_path_.string().c_str(), &database_,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX | SQLITE_OPEN_NOFOLLOW,
        nullptr);
    if (result != SQLITE_OK) {
      const auto detail = SqliteDetail(database_, result);
      if (database_ != nullptr) {
        sqlite3_close(database_);
        database_ = nullptr;
      }
      Fail("component_lifecycle_journal_storage_failure",
           "open lifecycle journal database: " + detail);
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

  void Close() {
    std::lock_guard lock(mutex_);
    if (closed_) {
      return;
    }
    sqlite3_wal_checkpoint_v2(database_, nullptr, SQLITE_CHECKPOINT_TRUNCATE, nullptr, nullptr);
    const auto result = sqlite3_close(database_);
    if (result != SQLITE_OK) {
      Fail("component_lifecycle_journal_storage_failure",
           "close lifecycle journal database: " + SqliteDetail(database_, result));
    }
    database_ = nullptr;
    closed_ = true;
  }

  CCLCJournalRecoveryDTO BeginOwnerEpoch(
      const RequestOwnerEpoch& owner_epoch, std::int64_t started_at_ns,
      std::size_t request_membership_capacity,
      const std::optional<RequestOwnerEpoch>& quiescent_predecessor,
      CCLCOwnerEpochTransitionPurpose purpose);

  std::optional<ComponentLifecycleTargetRetentionDTO> ReadLifecycleTargetRetention(
      const ControllableComponentId& component);

  std::vector<ComponentLifecycleTargetDTO> LoadLifecycleTargetsForClose(const RequestOwnerEpoch&);
  std::optional<CCLCLifecycleTargetRetirementResultDTO> ReadLifecycleTargetRetirementResult(
      const CCLCLifecycleTargetRetirementIntentDTO&);
  std::optional<CCLCLifecycleTargetRetirementResultDTO> FindTargetRetirementResultLocked(
      const CCLCLifecycleTargetRetirementIntentDTO&, std::string_view);
  CCLCLifecycleTargetRetirementResultDTO RetireLifecycleTargetsForClose(
      const RequestOwnerEpoch&, const CCLCLifecycleTargetRetirementIntentDTO&,
      const ICellCloseAdmissionLease&);
  void ReadQuiescentOwner(const RequestOwnerEpoch& expected);
  ApplyComponentGenerationSelectionDTO ReadApplyComponentGenerationSelection(
      const RequestOwnerEpoch&, const ControllableComponentId&,
      const std::optional<ApplyComponentQualifiedPredecessorDTO>&);

  std::optional<CCLCLifecycleRequestResultDTO> InspectLifecycleRequest(
      const RequestOwnerEpoch& active_owner_epoch, const ComponentLifecycleIntentDTO& intent,
      const ContentDigest* original_request_digest = nullptr, bool apply_delegated = false);

  std::optional<CCLCLifecycleRequestResultDTO> ReadLifecycleRequestResult(
      const ComponentLifecycleIntentDTO& intent,
      const ContentDigest* original_request_digest = nullptr, bool apply_delegated = false);

  CCLCLifecycleRequestResultDTO RecordLifecycleRejection(
      const RequestOwnerEpoch&, const ComponentLifecycleIntentDTO&, const ErrorDTO&,
      const std::optional<CellAdmissionClaimIntentDTO>&,
      const ContentDigest* original_request_digest = nullptr);

  CCLCLifecycleRequestResultDTO AcceptOrReplayLifecycle(
      const RequestOwnerEpoch& active_owner_epoch, const ComponentLifecycleIntentDTO& intent,
      const ComponentLifecycleAcceptedDTO& candidate_acceptance,
      const ComponentLifecycleStatusDTO& candidate_initial_status,
      const ContentDigest* original_request_digest = nullptr, bool apply_delegated = false,
      const ComponentLifecycleTargetExpectationDTO* expected_target = nullptr,
      const CellAdmissionClaimDTO* acquired_claim = nullptr);
  void RequireTargetClaimLocked(const RequestOwnerEpoch&, const ComponentLifecycleIntentDTO&,
                                const ComponentLifecycleAcceptedDTO&, const CellAdmissionClaimDTO&);
  void RecordCellClaimAttempt(const RequestOwnerEpoch&, const CellAdmissionClaimIntentDTO&,
                              const std::optional<ContentDigest>& = std::nullopt);
  std::optional<std::string> ReadClaimRecoveryLocked(const CellAdmissionClaimIntentDTO&);
  ContentDigest ReadOriginalRequestDigestLocked(std::string_view, const ContentDigest&);
  void RecordCellClaimAcquired(const RequestOwnerEpoch&, const CellAdmissionClaimDTO&);
  void RecordCellClaimReleaseReceipt(const RequestOwnerEpoch&, const CellAdmissionClaimReleaseDTO&,
                                     const CellAdmissionRecoveryReceiptDTO&);
  void RecordUnacceptedCellClaimRelease(const RequestOwnerEpoch&,
                                        const CellAdmissionClaimReleaseDTO&);
  void RecordUnacceptedCellClaimReleaseLocked(const RequestOwnerEpoch&,
                                             const CellAdmissionClaimReleaseDTO&);
  void RecordCellClaimNotAcquired(const RequestOwnerEpoch&, const CellAdmissionClaimIntentDTO&);

  std::uint64_t RecordEffectPrepared(const RequestOwnerEpoch& active_owner_epoch,
                                     const CCLCEffectAttemptDTO& prepared_attempt,
                                     const ComponentLifecycleStatusDTO& successor_status);

  std::uint64_t RecordEffectCompleted(const RequestOwnerEpoch& active_owner_epoch,
                                      const CCLCEffectAttemptDTO& completed_attempt,
                                      const ComponentLifecycleStatusDTO& successor_status);

  std::uint64_t RecordLifecycleStatus(const RequestOwnerEpoch& active_owner_epoch,
                                      const ComponentLifecycleStatusDTO& status,
                                      const CellAdmissionClaimReleaseDTO* release = nullptr);

  std::optional<ComponentLifecycleStatusDTO> LoadRetainedLifecycleStatus(
      const LifecycleTransactionId& lifecycle_transaction_id);

  ComponentLifecycleCurrentFactReadbackDTO LoadCurrentComponentLifecycleFact(
      const ControllableComponentId& controllable_component_id);

  CCLCCurrentFactsReadbackDTO LoadCurrentSnapshotFacts(
      const std::vector<ControllableComponentId>& canonical_controllable_component_ids,
      const std::vector<ControlDomainId>& canonical_control_domain_ids);

  std::optional<CCLCRouteSelectionRequestResultDTO> InspectRouteSelectionRequest(
      const RequestOwnerEpoch& active_owner_epoch, const RouteSelectionIntentDTO& intent);

  std::optional<CCLCRouteSelectionRequestResultDTO> ReadRouteSelectionRequestResult(
      const RouteSelectionIntentDTO& intent);

  CCLCRouteSelectionRequestResultDTO RecordRouteSelectionRejection(
      const RequestOwnerEpoch& owner, const RouteSelectionIntentDTO& intent, const ErrorDTO& error,
      const std::optional<CellAdmissionClaimIntentDTO>& unacquired);

  CCLCRouteSelectionRequestResultDTO AcceptOrReplayRouteSelection(
      const RequestOwnerEpoch& active_owner_epoch, const RouteSelectionIntentDTO& intent,
      const RouteSelectionAcceptedDTO& candidate_acceptance,
      const RouteSelectionStatusDTO& candidate_initial_status);

  std::uint64_t RecordRouteEffectPrepared(const RequestOwnerEpoch& active_owner_epoch,
                                          const CCLCRouteEffectAttemptDTO& prepared_attempt,
                                          const RouteSelectionStatusDTO& successor_status);

  std::uint64_t RecordRouteEffectCompleted(const RequestOwnerEpoch& active_owner_epoch,
                                           const CCLCRouteEffectAttemptDTO& completed_attempt,
                                           const RouteSelectionStatusDTO& successor_status,
                                           const CellAdmissionClaimReleaseDTO* release = nullptr);

  std::uint64_t RecordRouteSelectionStatus(const RequestOwnerEpoch& active_owner_epoch,
                                           const RouteSelectionStatusDTO& status,
                                           const CellAdmissionClaimReleaseDTO* release = nullptr);

  std::optional<RouteSelectionStatusDTO> LoadRetainedRouteSelectionStatus(
      const RouteSelectionTransactionId& route_selection_transaction_id);

  std::optional<ActiveControlConfigurationDTO> LoadCurrentActiveControlConfiguration(
      const ControlDomainId& control_domain_id);

  NativeApplyPlanDTO PlanNativeApply(const NativeApplyPlanRequestDTO& request);
  std::optional<NamedApplyPublicationTransactionDTO> ReadUnsettledNativeApplyPlan();
  std::vector<ControllableComponentId> LoadComponentsWithUnsettledLifecycle();
  std::optional<NativeApplyPlanDTO> LoadNativeApplyPlan(
      const NamedApplyPublicationTransactionDTO& apply);
  void AdoptNativeApplyFacts(const NativeApplyFactAdoptionDTO& adoption);
  bool HasNativeApplyFacts(const ActiveBundlePublicationReceiptDTO& publication);

  ComponentLifecycleTargetDTO SetLifecycleTarget(const ControllableComponentId& component,
                                                 LifecycleTargetStateDTO requested_state,
                                                 LifecycleTargetOriginDTO origin);
  std::vector<ComponentLifecycleTargetDTO> LoadLifecycleTargets();
  std::vector<ComponentLifecycleTargetDTO> LoadLifecycleTargetsLocked();
  std::vector<ComponentLifecycleTargetDTO> RetireLifecycleTargetsForClose(const RequestOwnerEpoch&);
  std::optional<ComponentLifecycleTargetDTO> LoadLifecycleTarget(
      const ControllableComponentId& component);
  ComponentLifecycleTargetFactsDTO LoadLifecycleTargetFacts(
      const ControllableComponentId& component);
  bool RecordInterventionCause(const ControllableComponentId& component,
                               const ControlDomainId& domain,
                               std::optional<LifecycleBlockingCauseClassDTO> cause_class,
                               std::int64_t observed_at_ns, std::uint64_t recovery_generation);
  std::vector<ComponentLifecycleInterventionCauseDTO> LoadInterventionCauses(
      const ControllableComponentId& component);
  bool RecordLifecycleTargetReached(const ControllableComponentId& component, bool reached,
                                    std::uint64_t recovery_generation,
                                    const ComponentLifecycleTargetExpectationDTO* = nullptr);
  bool ComponentInUnadoptedApplyPlan(const ControllableComponentId& component);
  CellObserverIncidentRecordDTO RecordCellObserverIncident(
      const CellObserverIncidentRecordDTO& record);
  std::optional<CellObserverIncidentRecordDTO> LoadOpenCellObserverIncident();
  std::optional<CellObserverLossAssociationDTO> LoadOpenCellObserverLossAssociation();
  std::optional<CellObserverBringUpRecordDTO> LoadLatestCellObserverBringUp();
  CellObserverBringUpAdmissionDTO BeginLatestCellObserverBringUp(
      const CellObserverBringUpRecordDTO&, const std::optional<CellObserverBringUpRecordDTO>&);
  CellObserverBringUpRecordDTO UpdateLatestCellObserverBringUp(const CellObserverBringUpRecordDTO&,
                                                               const CellObserverBringUpRecordDTO&);
  CellObserverBringUpRecordDTO CloseCellObserverIncidentByOperatorBringUp(
      const CellObserverBringUpRecordDTO&, const CellObserverBringUpOpenDTO&);
  ComponentIncidentRecordDTO OpenComponentIncident(const ComponentIncidentRecordDTO& record,
                                                   std::uint64_t recovery_generation);
  std::optional<ComponentIncidentRecordDTO> LoadOpenComponentIncident(
      const ControllableComponentId& component);
  bool RecordComponentIncidentContainment(const std::string& incident_id,
                                          ComponentIncidentContainmentStateDTO state,
                                          const std::string& record_json,
                                          std::int64_t occurred_at_ns);
  std::optional<CCLCLifecycleRequestResultDTO> RecoverComponentIncident(
      const RequestOwnerEpoch& active_owner_epoch, const ComponentLifecycleIntentDTO& intent,
      const std::string& acknowledged_by, std::int64_t acknowledged_at_ns);
  bool UpdateOpenComponentIncident(const std::string& incident_id,
                                   LifecycleBlockingCauseClassDTO cause_class,
                                   const std::vector<std::string>& vendor_codes,
                                   const std::string& record_json);
  bool AppendComponentIncidentDiagnostic(
      const std::string& incident_id, std::int64_t occurred_at_ns, const ErrorDTO& diagnostic,
      std::shared_ptr<const content_identity::CanonicalValue> snapshot);
  std::vector<ComponentIncidentEventDTO> LoadComponentIncidentEvents(std::uint64_t after_sequence,
                                                                     std::size_t limit);

  void RefreshNativeApplyPreparationFailure();

 private:
  std::optional<CellObserverIncidentRecordDTO> ReadOpenCellObserverIncidentLocked(
      CellObserverLossAssociationDTO* association = nullptr);
  std::optional<CellObserverBringUpRecordDTO> ReadLatestCellObserverBringUpLocked();
  void WriteLatestCellObserverBringUpLocked(const std::string&);
  ComponentIncidentPruneNotice PruneClosedIncidentsLocked();
  void NotifyIncidentPruning(const ComponentIncidentPruneNotice& notice) noexcept;
  std::optional<ComponentIncidentRecordDTO> ReadOpenComponentIncidentLocked(
      const ControllableComponentId& component, bool decode_diagnostic = true);
  std::vector<ComponentLifecycleInterventionCauseDTO> ReadInterventionCausesLocked(
      const ControllableComponentId& component);
  /// The Recover result retained for one request membership, if that
  /// request closed an incident; caller holds the mutex.
  std::optional<ComponentIncidentRecoveryDTO> ReadComponentIncidentRecoveryLocked(
      const PublicRequestContextDTO& request);
  /// Effects of an accepted-new operator request inside
  /// AcceptOrReplayLifecycle's write transaction: the operator target, or a
  /// Recover's clearing of retained intervention causes.
  void RecordAcceptedOperatorRequestLocked(const ComponentLifecycleIntentDTO& intent);
  std::string PreserveComponentIncidentDiagnosticLocked(const std::string& incident_id,
                                                        const std::string& record_json);
  void AppendComponentIncidentEventLocked(std::string_view event_kind,
                                          const std::string& incident_id,
                                          std::int64_t occurred_at_ns,
                                          const std::string& payload_json);
  /// The stored row, cancelled or not; caller holds the mutex.
  std::optional<std::pair<ComponentLifecycleTargetDTO, bool>> ReadLifecycleTargetLocked(
      const ControllableComponentId& component);
  std::optional<ComponentLifecycleTargetRetentionDTO> ReadLifecycleTargetRetentionLocked(
      const ControllableComponentId& component);
  /// Mint the next target generation for one component; caller holds the
  /// mutex inside a write transaction.
  /// `reset_reached` starts the component over (startup or changed by an
  /// Apply); an operator target keeps whether it reached its target.
  ComponentLifecycleTargetDTO WriteLifecycleTargetLocked(const ControllableComponentId& component,
                                                         LifecycleTargetStateDTO requested_state,
                                                         LifecycleTargetOriginDTO origin,
                                                         bool reset_reached);
  // Retire requested intent only: preserve target/recovery generations,
  // recovered causes, intervention causes and incident evidence.
  void RetireLifecycleTargetIntentLocked(const ControllableComponentId& component,
      LifecycleTargetRetirementReasonDTO reason = LifecycleTargetRetirementReasonDTO::kOrdinaryCancellation);
  /// Cancel the target and drop the intervention causes of a component that
  /// left the adopted set; its generation watermark stays.
  void CancelLifecycleTargetLocked(const ControllableComponentId& component);
  /// The component's recovery generation (0 before the first); caller holds
  /// the mutex.
  std::uint64_t ReadRecoveryGenerationLocked(const ControllableComponentId& component);
  /// Advance it inside the caller's write transaction: every commit that
  /// clears the driver's facts (Recover, cancellation) calls this.
  void AdvanceRecoveryGenerationLocked(const ControllableComponentId& component);
  /// Refuse a driver write based on facts read before the last advance.
  void RequireRecoveryGenerationLocked(const ControllableComponentId& component,
                                       std::uint64_t expected);
  void InsertRejection(const RequestOwnerEpoch& owner, const RequestId& request_id,
                       std::string_view kind, std::string_view digest, const ErrorDTO& error);
  void RecordRejectionLocked(const PublicRequestContextDTO& context, std::string_view kind,
                             std::string_view digest, const ErrorDTO& error,
                             const std::optional<CellAdmissionClaimIntentDTO>& unacquired,
                             const std::optional<ProgramNodeAdmissionEvidenceDTO>& evidence);
  void RetainCellClaimRelease(const CellAdmissionClaimReleaseDTO& release,
                              std::string_view transaction_id, std::string_view terminal_digest);
  void BindCellClaim(const PublicRequestContextDTO& request, std::string_view transaction_id,
                     AdmissionGeneration admission, std::string_view semantic_digest,
                     const std::optional<CellAdmissionConfigurationScopeDTO>& recovery = {});
  void BindSelectedCellClaim(const ComponentLifecycleStatusDTO& status);
  void RequireApplyBringUpAcceptanceLocked(const ComponentLifecycleStatusDTO& status);
  void RetainRecoveredNoAdmission(const RequestOwnerEpoch& owner, std::string_view transaction_id,
                                  const RequestId& request_id, const ContentDigest& digest);
  /// The current plan when it belongs to ``apply``; caller holds the mutex.
  std::optional<NativeApplyPlanDTO> ReadNativeApplyPlanLocked(
      const NamedApplyPublicationTransactionDTO& apply);
  /// Highest generation already used or reserved for one identity.
  std::uint64_t NativeApplyGenerationFloorLocked(std::string_view kind,
                                                 std::string_view identity);
  std::pair<std::uint64_t, std::uint64_t> ReadNativeApplyGenerationValuesLocked(
      std::string_view kind, std::string_view identity);
  ApplyComponentGenerationSelectionDTO SelectApplyComponentGenerationLocked(std::string_view identity,
      const std::optional<ApplyComponentQualifiedPredecessorDTO>& qualified = std::nullopt);
  std::map<ControllableComponentId, ComponentLifecycleStatusDTO> ReadNativeApplyChildrenLocked(
      const NativeApplyComponentBringUpDTO&, const NamedApplyPublicationTransactionDTO&,
      const RetainedBundleId&, const GraphRevision&, bool unstarted,
      const ActiveBundlePublicationReceiptDTO* publication = nullptr);
  void RequireNativeApplyCapacityLocked(
      const std::vector<ControllableComponentId>& components,
      const std::vector<ControlDomainId>& domains);
  std::optional<NamedApplyPublicationTransactionDTO> ReadUnsettledNativeApplyPlanLocked();
  std::uint64_t ActiveTransactionCount() {
    return Nonnegative(
        ScalarInteger(
            database_,
            "WITH records(id,terminal) AS ("
            "SELECT lifecycle_transaction_id,terminal FROM lifecycle_transactions UNION ALL "
            "SELECT route_selection_transaction_id,terminal FROM route_selection_transactions) "
            "SELECT count(*) FROM records t WHERE terminal=0 OR EXISTS ("
            "SELECT 1 FROM cclc_lifecycle_cell_claims c WHERE c.lifecycle_transaction_id=t.id "
            "AND c.release_receipt_recorded=0)"),
        "active CCLC transaction count");
  }
  void RetainCurrentComponentLifecycleFact(const ComponentLifecycleStatusDTO& status);
  void RetainCurrentActiveControlConfiguration(const RouteSelectionStatusDTO& status);

  void RequireOpen() const {
    Require(!closed_ && database_ != nullptr, "component_lifecycle_journal_closed",
            "lifecycle journal database is closed");
  }

  void Configure() {
    Check(database_, sqlite3_busy_timeout(database_, kBusyTimeoutMilliseconds),
          "configure lifecycle journal busy timeout");
    Exec(database_, "PRAGMA journal_mode=WAL");
    Exec(database_, "PRAGMA synchronous=FULL");
    Exec(database_, "PRAGMA foreign_keys=ON");
    Exec(database_, "PRAGMA trusted_schema=OFF");
    Exec(database_, "PRAGMA recursive_triggers=OFF");
    Exec(database_, "PRAGMA wal_autocheckpoint=256");
    Require(ScalarInteger(database_, "PRAGMA journal_size_limit=" +
                                         std::to_string(kMaximumRetainedWalBytes)) ==
                kMaximumRetainedWalBytes,
            "component_lifecycle_journal_storage_failure",
            "CCLC journal WAL ceiling could not be configured");
    int defensive_enabled = 0;
    Check(database_, sqlite3_db_config(database_, SQLITE_DBCONFIG_DEFENSIVE, 1, &defensive_enabled),
          "enable defensive lifecycle journal connection");
    Require(defensive_enabled == 1, "component_lifecycle_journal_storage_unsupported",
            "SQLite defensive mode could not be enabled");
    const auto page_size = ScalarInteger(database_, "PRAGMA page_size");
    Require(page_size > 0, "component_lifecycle_journal_corrupt",
            "lifecycle journal SQLite page size is invalid");
    const auto maximum_pages =
        std::max<std::int64_t>(1, kMaximumDatabasePageImageBytes / page_size);
    Require(ScalarInteger(database_, "PRAGMA max_page_count=" + std::to_string(maximum_pages)) ==
                maximum_pages,
            "component_lifecycle_journal_storage_failure",
            "lifecycle journal SQLite page ceiling could not be configured");
    sqlite3_limit(database_, SQLITE_LIMIT_LENGTH, static_cast<int>(kMaximumSqliteEncodedRowBytes));
    Require(sqlite3_limit(database_, SQLITE_LIMIT_LENGTH, -1) >=
                static_cast<int>(kMaximumSqliteEncodedRowBytes),
            "component_lifecycle_journal_storage_unsupported",
            "SQLite encoded-row limit is below the CCLC journal contract");
  }

  void InitializeOrValidate() {
    std::lock_guard lock(mutex_);
    Transaction transaction(database_);
    const auto objects = SchemaObjectNames(database_);
    if (objects.empty()) {
      Require(ScalarInteger(database_, "PRAGMA application_id") == 0 &&
                  ScalarInteger(database_, "PRAGMA user_version") == 0,
              "component_lifecycle_journal_storage_foreign",
              "schema-empty lifecycle journal has a foreign identity");
      Exec(database_, kSchema);
      Exec(database_, kProgramNodeSchemaUpgrade);
      Exec(database_, kRouteRecoverySchemaUpgrade);
      Exec(database_, kNativeApplySchemaUpgrade);
      Exec(database_, kNativeApplyDeltaSchemaUpgrade);
      Exec(database_, kLifecycleTargetSchemaUpgrade);
      Exec(database_, "PRAGMA application_id=" + std::to_string(kCCLCJournalSqliteApplicationId));
      Exec(database_, "PRAGMA user_version=" + std::to_string(kCCLCJournalSchemaVersion));
      Statement metadata(database_,
                         "INSERT INTO cclc_journal_metadata("
                         "singleton,schema_id,schema_version,cell_id,"
                         "exact_result_history_capacity,terminal_result_bytes_capacity,"
                         "highest_transaction_generation,"
                         "highest_route_selection_transaction_generation,"
                         "highest_terminal_sequence,observer_latest_bring_up) "
                         "VALUES(1,?,?,?,?,?,0,0,0,NULL)");
      metadata.Bind(1, kCCLCJournalSchemaId);
      metadata.Bind(2, kCCLCJournalSchemaVersion);
      metadata.Bind(3, cell_id_.value());
      metadata.Bind(4, static_cast<std::int64_t>(exact_result_history_capacity_));
      metadata.Bind(5, static_cast<std::int64_t>(terminal_result_bytes_capacity_));
      Require(!metadata.Step(), "component_lifecycle_journal_corrupt",
              "lifecycle journal metadata insert returned an unexpected row");
    }
    // Current-only development journal: earlier versions are not migrated.
    // Version 19 distinguishes ordinary cancellation from Close on the same
    // retained target row. Version 18 added bounded target-retirement results
    // and the existing metadata singleton's observer latest-attempt value.
    // Version 17 added the per-component recovery generation that fences the
    // lifecycle-target driver's writes against a concurrent operator Recover.
    // Version 16 names the operator's incident Recover by its ordinary request
    // identity: the acknowledgement_owner_epoch column, the acknowledgement
    // CHECK and the unique (owner epoch, request id) recovery index.
    // Version 15 added component incidents, their history events and the
    // reached-target marker; version 14 added requested lifecycle targets and
    // intervention causes.
    // Version 13 removed the Disable Stop-handoff effect kinds and status
    // fields, so an older journal is refused unchanged before any write.
    if (const auto found_version = ScalarInteger(database_, "PRAGMA user_version");
        ScalarInteger(database_, "PRAGMA application_id") == kCCLCJournalSqliteApplicationId &&
        found_version != kCCLCJournalSchemaVersion) {
      Fail("component_lifecycle_journal_storage_unsupported",
           "CCLC journal schema version " + std::to_string(found_version) +
               " is unsupported; this owner opens only version " +
               std::to_string(kCCLCJournalSchemaVersion) +
               " and does not migrate earlier lifecycle history. The journal was left "
               "unchanged. Stop the development workbench (./scripts/compose-dev.sh stop) "
               "and start it again for fresh runtime state.");
    }
    Require(SchemaObjectNames(database_) == ExpectedSchemaObjects() &&
                SchemaDefinitions(database_) == ExpectedSchemaDefinitions(),
            "component_lifecycle_journal_storage_foreign",
            "lifecycle journal file " + database_path_.string() +
                " has an unsupported earlier-draft or foreign layout; this owner requires the "
                "current component/cell-observer incident and linked Native Apply layout. File left unchanged; obtain "
                "a compatible runtime/layout before opening it.");
    Require(ScalarInteger(database_, "PRAGMA application_id") == kCCLCJournalSqliteApplicationId &&
                ScalarInteger(database_, "PRAGMA user_version") == kCCLCJournalSchemaVersion,
            "component_lifecycle_journal_storage_foreign",
            "lifecycle journal SQLite identity is invalid");
    Statement metadata(database_,
                       "SELECT schema_id,schema_version,cell_id,"
                       "exact_result_history_capacity,terminal_result_bytes_capacity "
                       "FROM cclc_journal_metadata "
                       "WHERE singleton=1");
    Require(metadata.Step() && metadata.Text(0) == kCCLCJournalSchemaId &&
                metadata.Integer(1) == kCCLCJournalSchemaVersion &&
                metadata.Text(2) == cell_id_.value() &&
                metadata.Integer(3) == static_cast<std::int64_t>(exact_result_history_capacity_) &&
                metadata.Integer(4) == static_cast<std::int64_t>(terminal_result_bytes_capacity_) &&
                !metadata.Step(),
            "component_lifecycle_journal_storage_foreign",
            "lifecycle journal metadata is invalid or configuration changed");
    Statement quick_check(database_, "PRAGMA quick_check");
    Require(quick_check.Step() && quick_check.Text(0) == "ok" && !quick_check.Step(),
            "component_lifecycle_journal_corrupt", "lifecycle journal SQLite quick check failed");
    Statement foreign_key_check(database_, "PRAGMA foreign_key_check");
    Require(!foreign_key_check.Step(), "component_lifecycle_journal_corrupt",
            "lifecycle journal has a foreign-key violation");
    ValidateRetainedState();
    // Validate every historical guard before removing any. A noncanonical
    // legacy ID could otherwise collide with a future counter-minted identity.
    RetireQuiescentRequestNamespaces();
    transaction.Commit();
  }

  [[nodiscard]] bool ActiveOwner(const RequestOwnerEpoch& owner_epoch) {
    Statement owner(database_,
                    "SELECT 1 FROM cclc_owner_epochs "
                    "WHERE owner_epoch=? AND active=1");
    owner.Bind(1, owner_epoch.value());
    const bool found = owner.Step();
    Require(!owner.Step(), "component_lifecycle_journal_corrupt",
            "lifecycle journal has duplicate active owner rows");
    return found;
  }

  enum class OwnerQuiescence { kOwnerMismatch, kUnsettled, kQuiescent };

  [[nodiscard]] OwnerQuiescence ReadOwnerQuiescenceLocked(const RequestOwnerEpoch& expected) {
    if (!ActiveOwner(expected)) return OwnerQuiescence::kOwnerMismatch;
    const auto unsettled = ScalarInteger(
        database_,
        "SELECT (SELECT count(*) FROM lifecycle_transactions WHERE terminal=0)+"
        "(SELECT count(*) FROM route_selection_transactions WHERE terminal=0)+"
        "(SELECT count(*) FROM cclc_lifecycle_cell_claims WHERE release_receipt_recorded=0)+"
        "(SELECT count(*) FROM cclc_effect_attempts WHERE completed_attempt_blob IS NULL)+"
        "(SELECT count(*) FROM cclc_route_effect_attempts WHERE completed_attempt_blob IS NULL)");
    return unsettled == 0 ? OwnerQuiescence::kQuiescent : OwnerQuiescence::kUnsettled;
  }

  [[nodiscard]] std::uint64_t MintTerminalSequence() {
    Statement metadata(database_,
                       "SELECT highest_terminal_sequence FROM cclc_journal_metadata "
                       "WHERE singleton=1");
    Require(metadata.Step(), "component_lifecycle_journal_corrupt",
            "terminal sequence watermark is missing");
    const auto highest = Nonnegative(metadata.Integer(0), "highest terminal sequence");
    Require(!metadata.Step() &&
                highest < static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()),
            "component_lifecycle_journal_generation_exhausted",
            "terminal sequence watermark is exhausted or duplicated");
    const auto next = highest + 1U;
    Statement update(database_,
                     "UPDATE cclc_journal_metadata SET highest_terminal_sequence=? "
                     "WHERE singleton=1");
    update.Bind(1, static_cast<std::int64_t>(next));
    Require(!update.Step() && sqlite3_changes(database_) == 1,
            "component_lifecycle_journal_corrupt", "terminal sequence watermark did not advance");
    return next;
  }

  void RetireQuiescentRequestNamespaces() {
    // Current-namespace tombstones prevent public request-ID reuse. Once that
    // namespace is retired they can go individually: one retained result must
    // not pin every expired identity from the same epoch.
    // Keep exactly the newest complete full predecessor. Its membership set
    // is bounded by M and proves that an absent request never acquired an
    // effect, even when a capacity-deferral response was lost. Other old
    // namespaces retain only their ordinary bounded result/claim references.
    Exec(database_,
         "DELETE FROM cclc_request_memberships WHERE retained_result=0 "
         "AND owner_epoch IN (SELECT owner_epoch FROM cclc_owner_epochs WHERE active=0) "
         "AND owner_epoch IS NOT (SELECT e.owner_epoch FROM cclc_owner_epochs e "
         "WHERE active=0 AND request_membership_capacity=(SELECT count(*) "
         "FROM cclc_request_memberships m WHERE m.owner_epoch=e.owner_epoch) "
         "ORDER BY epoch_sequence DESC LIMIT 1) "
         "AND NOT EXISTS (SELECT 1 FROM cclc_lifecycle_cell_claims c WHERE "
         "c.owner_epoch=cclc_request_memberships.owner_epoch AND "
         "c.request_id=cclc_request_memberships.request_id)");
    Exec(database_,
         "DELETE FROM cclc_owner_epochs WHERE active=0 "
         "AND NOT EXISTS (SELECT 1 FROM cclc_request_memberships m WHERE "
         "m.owner_epoch=cclc_owner_epochs.owner_epoch) "
         "AND NOT EXISTS (SELECT 1 FROM cclc_lifecycle_cell_claims c WHERE "
         "c.owner_epoch=cclc_owner_epochs.owner_epoch) "
         "AND NOT EXISTS (SELECT 1 FROM cclc_effect_attempts a WHERE "
         "a.prepared_owner_epoch=cclc_owner_epochs.owner_epoch OR "
         "a.completed_owner_epoch=cclc_owner_epochs.owner_epoch) "
         "AND NOT EXISTS (SELECT 1 FROM cclc_route_effect_attempts a WHERE "
         "a.prepared_owner_epoch=cclc_owner_epochs.owner_epoch OR "
         "a.completed_owner_epoch=cclc_owner_epochs.owner_epoch)");
    // Non-reuse is enforced by the durable counter and canonical ID below.
    // Guard rows now serve only retained-result/configuration referential proof.
    Exec(database_,
         "DELETE FROM cclc_lifecycle_transaction_identity_guards WHERE "
         "NOT EXISTS (SELECT 1 FROM lifecycle_transactions t WHERE "
         "t.lifecycle_transaction_id=cclc_lifecycle_transaction_identity_guards.lifecycle_"
         "transaction_id)");
    Exec(database_,
         "DELETE FROM cclc_route_transaction_identity_guards WHERE "
         "NOT EXISTS (SELECT 1 FROM route_selection_transactions t WHERE "
         "t.route_selection_transaction_id=cclc_route_transaction_identity_guards.route_selection_"
         "transaction_id) "
         "AND NOT EXISTS (SELECT 1 FROM control_domain_active_configurations c WHERE "
         "c.origin_route_selection_transaction_id=cclc_route_transaction_identity_guards.route_"
         "selection_transaction_id)");
  }

  void ValidateRetainedState();

  std::uint64_t OwnerSequence() {
    const auto retained = Nonnegative(
        ScalarInteger(database_, "SELECT coalesce(max(epoch_sequence),0) FROM cclc_owner_epochs"),
        "retained owner sequence");
    Statement counter(database_, "SELECT seq FROM sqlite_sequence WHERE name='cclc_owner_epochs'");
    if (!counter.Step()) {
      Require(retained == 0, "component_lifecycle_journal_corrupt",
              "request namespace counter is missing");
      return 0U;
    }
    const auto sequence = Nonnegative(counter.Integer(0), "request namespace high-watermark");
    Require(sequence >= retained && sequence <= 9'007'199'254'740'991ULL && !counter.Step(),
            "component_lifecycle_journal_corrupt",
            "request namespace counter regressed or is duplicated");
    return sequence;
  }
  void CompactTerminalResults(std::size_t prospective_terminal_count = 0U,
                              std::uint64_t prospective_terminal_bytes = 0U);
  std::optional<CCLCLifecycleRequestResultDTO> FindLifecycleRequestResultLocked(
      const ComponentLifecycleIntentDTO& intent, std::string_view semantic_digest);
  bool MembershipFull(const RequestOwnerEpoch& owner) {
    Statement remaining(
        database_,
        "SELECT request_membership_capacity-(SELECT count(*) FROM cclc_request_memberships m "
        "WHERE m.owner_epoch=e.owner_epoch) FROM cclc_owner_epochs e WHERE owner_epoch=?");
    remaining.Bind(1, owner.value());
    if (!remaining.Step()) return false;
    const auto available = remaining.Integer(0);
    Require(available >= 0 && !remaining.Step(), "component_lifecycle_journal_corrupt",
            "active owner membership exceeds its bound");
    return available == 0;
  }

  std::optional<CCLCLifecycleRequestResultDTO> InspectLifecycleRequestLocked(
      const RequestOwnerEpoch& active_owner_epoch, const ComponentLifecycleIntentDTO& intent,
      std::string_view semantic_digest);
  std::optional<CCLCRouteSelectionRequestResultDTO> InspectRouteSelectionRequestLocked(
      const RequestOwnerEpoch& active_owner_epoch, const RouteSelectionIntentDTO& intent,
      std::string_view semantic_digest);
  std::optional<CCLCRouteSelectionRequestResultDTO> FindRouteSelectionRequestResultLocked(
      const RouteSelectionIntentDTO& intent, std::string_view semantic_digest);
  bool UnstartedInFullNamespace(const PublicRequestContextDTO& request) {
    if (!MembershipFull(request.request_owner_epoch)) return false;
    Statement crossing(
        database_, "SELECT 1 FROM cclc_lifecycle_cell_claims WHERE owner_epoch=? AND request_id=?");
    crossing.Bind(1, request.request_owner_epoch.value());
    crossing.Bind(2, request.request_id.value());
    return !crossing.Step();
  }
  void RequireRouteEffectProjection(const RouteSelectionStatusDTO& status);

  std::filesystem::path database_path_;
  CellId cell_id_;
  std::size_t exact_result_history_capacity_;
  std::size_t terminal_result_bytes_capacity_;
  std::function<void(const ComponentIncidentPruneNotice&)> on_incidents_pruned_;
  NativeApplyParentPreparationFailureSource preparation_failure_source_;
  std::optional<NativeApplyPlanDTO> observed_preparation_failure_;
  std::mutex mutex_;
  sqlite3* database_{};
  bool closed_{};
};

void SqliteCCLCJournal::Impl::ValidateRetainedState() {
  static_cast<void>(OwnerSequence());
  static_cast<void>(ReadLatestCellObserverBringUpLocked());
  Statement metadata(database_,
                     "SELECT highest_transaction_generation,"
                     "highest_route_selection_transaction_generation,"
                     "highest_terminal_sequence "
                     "FROM cclc_journal_metadata WHERE singleton=1");
  Require(metadata.Step(), "component_lifecycle_journal_corrupt",
          "lifecycle journal watermarks are missing");
  const auto highest_transaction_generation =
      Nonnegative(metadata.Integer(0), "highest transaction generation");
  const auto highest_route_selection_transaction_generation =
      Nonnegative(metadata.Integer(1), "highest route-selection transaction generation");
  const auto highest_terminal_sequence =
      Nonnegative(metadata.Integer(2), "highest terminal sequence");
  Require(!metadata.Step(), "component_lifecycle_journal_corrupt",
          "lifecycle journal watermarks are duplicated");

  Statement owners(database_,
                   "SELECT owner_epoch,started_at_ns,request_membership_capacity,active "
                   "FROM cclc_owner_epochs ORDER BY epoch_sequence");
  std::size_t active_owner_count = 0U;
  while (owners.Step()) {
    const auto owner_epoch = owners.Text(0);
    const auto started_at_ns = owners.Integer(1);
    const auto membership_capacity =
        Positive(owners.Integer(2), "owner request-membership capacity");
    const auto active = owners.Integer(3);
    Require(!owner_epoch.empty() && owner_epoch.size() <= kMaxLifecycleIdentityBytes &&
                started_at_ns >= 0 && membership_capacity <= kMaximumCCLCRequestMembership &&
                (active == 0 || active == 1),
            "component_lifecycle_journal_corrupt", "lifecycle journal owner row is invalid");
    active_owner_count += active == 1 ? 1U : 0U;
  }
  Require(active_owner_count <= 1U, "component_lifecycle_journal_corrupt",
          "lifecycle journal has multiple active owners");

  std::map<std::string, std::uint64_t> component_watermarks;
  Statement component_rows(
      database_,
      "SELECT controllable_component_id,component_lifecycle_generation "
      "FROM lifecycle_component_watermarks ORDER BY controllable_component_id");
  while (component_rows.Step()) {
    const auto component_id = component_rows.Text(0);
    const auto generation = Positive(component_rows.Integer(1), "component lifecycle watermark");
    Require(!component_id.empty() && component_id.size() <= kMaxLifecycleIdentityBytes &&
                component_watermarks.emplace(component_id, generation).second,
            "component_lifecycle_journal_corrupt",
            "component lifecycle watermark row is invalid or duplicated");
  }
  Require(component_watermarks.size() <= kMaximumCCLCComponentWatermarks,
          "component_lifecycle_journal_corrupt",
          "component lifecycle watermark capacity is exceeded");

  std::map<std::string, ComponentLifecycleStateDTO> current_component_facts;
  Statement current_component_rows(
      database_,
      "SELECT controllable_component_id,component_lifecycle_generation,"
      "component_lifecycle_state FROM lifecycle_component_current_facts "
      "ORDER BY controllable_component_id");
  while (current_component_rows.Step()) {
    const auto component_id = current_component_rows.Text(0);
    const auto generation =
        Positive(current_component_rows.Integer(1), "current component lifecycle generation");
    const auto raw_state = current_component_rows.Integer(2);
    const auto watermark = component_watermarks.find(component_id);
    const auto state = static_cast<ComponentLifecycleStateDTO>(raw_state);
    Require(watermark != component_watermarks.end() && watermark->second == generation &&
                raw_state >= 0 && raw_state <= 7 && !to_string(state).empty() &&
                current_component_facts.emplace(component_id, state).second,
            "component_lifecycle_journal_corrupt",
            "current component lifecycle fact is invalid or stale");
  }
  Require(current_component_facts.size() <= kMaximumCCLCComponentWatermarks,
          "component_lifecycle_journal_corrupt",
          "current component lifecycle fact capacity is exceeded");

  Statement retained_targets(database_,
      "SELECT controllable_component_id FROM cclc_component_lifecycle_targets");
  while (retained_targets.Step()) {
    const ControllableComponentId component(retained_targets.Text(0));
    Require(!component.empty() && component.value().size() <= kMaxLifecycleIdentityBytes &&
                ReadLifecycleTargetRetentionLocked(component).has_value(),
            "component_lifecycle_journal_corrupt", "retained target proof is invalid");
  }

  std::map<std::string, std::uint64_t> transaction_identity_guards;
  Statement identity_guards(database_,
                            "SELECT lifecycle_transaction_id,lifecycle_transaction_generation "
                            "FROM cclc_lifecycle_transaction_identity_guards "
                            "ORDER BY lifecycle_transaction_generation");
  while (identity_guards.Step()) {
    const auto transaction_id = identity_guards.Text(0);
    const auto transaction_generation =
        Positive(identity_guards.Integer(1), "guarded lifecycle transaction generation");
    Require(transaction_id == "lifecycle." + std::to_string(transaction_generation) ||
                transaction_id == "selected-lifecycle." + std::to_string(transaction_generation) ||
                transaction_id == "apply-lifecycle." + std::to_string(transaction_generation),
            "component_lifecycle_journal_identity_minting_unsupported",
            "historical lifecycle identity does not match the producer counter; migration refused");
    Require(!transaction_id.empty() && transaction_id.size() <= kMaxLifecycleIdentityBytes &&
                transaction_generation <= highest_transaction_generation &&
                transaction_identity_guards.emplace(transaction_id, transaction_generation).second,
            "component_lifecycle_journal_corrupt",
            "lifecycle transaction identity guard is invalid or duplicated");
  }

  std::map<std::string, std::uint64_t> active_configuration_watermarks;
  Statement active_configuration_rows(
      database_,
      "SELECT control_domain_id,active_control_configuration_generation "
      "FROM control_domain_active_configuration_watermarks ORDER BY control_domain_id");
  while (active_configuration_rows.Step()) {
    const auto control_domain_id = active_configuration_rows.Text(0);
    const auto generation =
        Positive(active_configuration_rows.Integer(1), "active-configuration watermark");
    Require(!control_domain_id.empty() &&
                control_domain_id.size() <= kMaxRouteSelectionIdentityBytes &&
                active_configuration_watermarks.emplace(control_domain_id, generation).second,
            "component_lifecycle_journal_corrupt",
            "active-configuration watermark row is invalid or duplicated");
  }
  Require(active_configuration_watermarks.size() <= kMaximumCCLCActiveConfigurationWatermarks,
          "component_lifecycle_journal_corrupt",
          "active-configuration watermark capacity is exceeded");

  std::map<std::string, std::uint64_t> route_transaction_identity_guards;
  Statement route_identity_guards(
      database_,
      "SELECT route_selection_transaction_id,route_selection_transaction_generation "
      "FROM cclc_route_transaction_identity_guards "
      "ORDER BY route_selection_transaction_generation");
  while (route_identity_guards.Step()) {
    const auto transaction_id = route_identity_guards.Text(0);
    const auto transaction_generation = Positive(route_identity_guards.Integer(1),
                                                 "guarded route-selection transaction generation");
    Require(transaction_id == "route-selection." + std::to_string(transaction_generation),
            "component_lifecycle_journal_identity_minting_unsupported",
            "historical route identity does not match the producer counter; migration refused");
    Require(!transaction_id.empty() && transaction_id.size() <= kMaxRouteSelectionIdentityBytes &&
                transaction_generation <= highest_route_selection_transaction_generation &&
                route_transaction_identity_guards.emplace(transaction_id, transaction_generation)
                    .second,
            "component_lifecycle_journal_corrupt",
            "route-selection transaction identity guard is invalid or duplicated");
  }

  Statement current_active_configurations(
      database_,
      "SELECT control_domain_id,active_control_configuration_generation,"
      "origin_route_selection_transaction_id,"
      "origin_route_selection_transaction_generation,configuration_blob "
      "FROM control_domain_active_configurations ORDER BY control_domain_id");
  std::size_t current_active_configuration_count = 0U;
  while (current_active_configurations.Step()) {
    const auto control_domain_id = current_active_configurations.Text(0);
    const auto generation = Positive(current_active_configurations.Integer(1),
                                     "current active-configuration generation");
    const bool has_origin = !current_active_configurations.IsNull(2);
    Require(has_origin == !current_active_configurations.IsNull(3),
            "component_lifecycle_journal_corrupt",
            "current active-configuration origin is only partly present");
    const auto configuration =
        detail::DecodeActiveControlConfiguration(current_active_configurations.Blob(4));
    const auto watermark = active_configuration_watermarks.find(control_domain_id);
    const auto origin =
        has_origin ? route_transaction_identity_guards.find(current_active_configurations.Text(2))
                   : route_transaction_identity_guards.end();
    Require(configuration.control_domain_id.value() == control_domain_id &&
                configuration.active_control_configuration_generation.value() == generation &&
                watermark != active_configuration_watermarks.end() &&
                generation <= watermark->second &&
                (!has_origin ||
                 (origin != route_transaction_identity_guards.end() &&
                  origin->second ==
                      Positive(current_active_configurations.Integer(3),
                               "current active-configuration origin transaction generation"))),
            "component_lifecycle_journal_corrupt",
            "current active-control-configuration publication is invalid");
    ++current_active_configuration_count;
  }
  Require(current_active_configuration_count <= kMaximumCCLCActiveConfigurationWatermarks,
          "component_lifecycle_journal_corrupt",
          "current active-control-configuration capacity is exceeded");

  Statement membership_consistency(
      database_,
      // A retained accepted lifecycle result is exactly one transaction or
      // one incident recovery (the operator's authorization-only Recover).
      "SELECT count(*) FROM cclc_request_memberships m LEFT JOIN "
      "lifecycle_transactions t ON t.membership_sequence="
      "m.membership_sequence LEFT JOIN route_selection_transactions "
      "r ON r.membership_sequence=m.membership_sequence LEFT JOIN "
      "cclc_component_incidents i ON i.target_kind='component' AND i.acknowledged=1 AND "
      "i.acknowledgement_owner_epoch=m.owner_epoch AND "
      "i.acknowledgement_request_id=m.request_id WHERE "
      "(m.retained_result=1 AND m.rejection_blob IS NULL AND "
      "((m.request_kind='component_lifecycle' "
      "AND ((t.transaction_sequence IS NULL) = (i.incident_id IS NULL) OR "
      "r.transaction_sequence IS NOT NULL)) OR "
      "(m.request_kind='route_selection' AND "
      "(r.transaction_sequence IS NULL OR "
      "t.transaction_sequence IS NOT NULL OR i.incident_id IS NOT NULL)))) OR "
      "(m.retained_result=0 AND (t.transaction_sequence IS NOT NULL "
      "OR r.transaction_sequence IS NOT NULL OR i.incident_id IS NOT NULL)) OR "
      "(m.rejection_blob IS NOT NULL AND (m.retained_result<>1 OR "
      "m.rejection_sequence IS NULL OR t.transaction_sequence IS NOT NULL OR "
      "r.transaction_sequence IS NOT NULL OR i.incident_id IS NOT NULL)) OR "
      "(m.rejection_blob IS NULL AND m.rejection_sequence IS NOT NULL) OR "
      "(m.request_kind='lifecycle_target_retirement' AND m.retained_result=1 "
      "AND m.target_retirement_blob IS NULL) OR "
      "(m.target_retirement_blob IS NOT NULL AND "
      "(m.request_kind<>'lifecycle_target_retirement' OR m.retained_result<>1 OR "
      "m.target_retirement_sequence IS NULL OR m.rejection_blob IS NOT NULL OR "
      "t.transaction_sequence IS NOT NULL OR r.transaction_sequence IS NOT NULL OR "
      "i.incident_id IS NOT NULL)) OR "
      "(m.target_retirement_blob IS NULL AND m.target_retirement_sequence IS NOT NULL)");
  Require(membership_consistency.Step() && membership_consistency.Integer(0) == 0 &&
              !membership_consistency.Step(),
          "component_lifecycle_journal_corrupt",
          "request membership and retained-result rows disagree");
  // A retirement shares the global terminal sequence allocator. Reject a
  // collision involving this new result family without changing validation
  // of historical result families against each other.
  Require(ScalarInteger(database_,
      "SELECT count(*) FROM (SELECT terminal_sequence FROM ("
      "SELECT terminal_sequence,0 AS retirement FROM lifecycle_transactions WHERE terminal_sequence IS NOT NULL "
      "UNION ALL SELECT terminal_sequence,0 FROM route_selection_transactions WHERE terminal_sequence IS NOT NULL "
      "UNION ALL SELECT rejection_sequence,0 FROM cclc_request_memberships WHERE rejection_sequence IS NOT NULL "
      "UNION ALL SELECT target_retirement_sequence,1 FROM cclc_request_memberships WHERE target_retirement_sequence IS NOT NULL"
      ") GROUP BY terminal_sequence HAVING count(*)>1 AND sum(retirement)>0)") == 0,
      "component_lifecycle_journal_corrupt", "target retirement terminal order is duplicated");
  // Every retained Recover keeps its ordinary request membership.
  Require(ScalarInteger(database_,
                        "SELECT count(*) FROM cclc_component_incidents i WHERE "
                        "i.target_kind='component' AND i.acknowledged=1 "
                        "AND NOT EXISTS (SELECT 1 FROM cclc_request_memberships m WHERE "
                        "m.owner_epoch=i.acknowledgement_owner_epoch AND "
                        "m.request_id=i.acknowledgement_request_id AND "
                        "m.request_kind='component_lifecycle' AND m.retained_result=1 AND "
                        "m.rejection_blob IS NULL)") == 0,
          "component_lifecycle_journal_corrupt",
          "an incident recovery has no retained request membership");

  Statement memberships(
      database_,
      "SELECT owner_epoch,request_id,request_kind,semantic_request_digest,"
      "retained_result,rejection_blob,rejection_sequence,target_retirement_blob,target_retirement_sequence "
      "FROM cclc_request_memberships "
      "ORDER BY membership_sequence");
  while (memberships.Step()) {
    const auto owner_epoch = memberships.Text(0);
    const auto request_id = memberships.Text(1);
    const auto request_kind = memberships.Text(2);
    const auto semantic_digest = memberships.Text(3);
    const auto retained_result = memberships.Integer(4);
    if (!memberships.IsNull(5)) {
      static_cast<void>(detail::DecodeCCLCAdmissionRejection(memberships.Blob(5)));
      Require((request_kind == "component_lifecycle" || request_kind == "route_selection") &&
                  Positive(memberships.Integer(6), "rejection terminal sequence") <=
                      highest_terminal_sequence,
              "component_lifecycle_journal_corrupt", "rejection terminal order is invalid");
    }
    if (!memberships.IsNull(7)) {
      const auto blob = memberships.Blob(7);
      const auto record = detail::DecodeCCLCTargetRetirementRecord(blob);
      Require(request_kind == "lifecycle_target_retirement" &&
                  record.intent.request_context.request_owner_epoch.value() == owner_epoch &&
                  record.intent.request_context.request_id.value() == request_id &&
                  detail::CCLCTargetRetirementSemanticIntentDigest(record.intent) == semantic_digest &&
                  detail::EncodeCCLCTargetRetirementRecord(record) == blob &&
                  Positive(memberships.Integer(8), "target retirement terminal sequence") <= highest_terminal_sequence,
              "component_lifecycle_journal_corrupt", "target retirement membership result is invalid");
    }
    Require(!owner_epoch.empty() && owner_epoch.size() <= kMaxLifecycleIdentityBytes &&
                !request_id.empty() && request_id.size() <= kMaxLifecycleIdentityBytes &&
                (request_kind == "component_lifecycle" || request_kind == "route_selection" ||
                 request_kind == "lifecycle_target_retirement") &&
                content_identity::is_sha256_digest(semantic_digest) &&
                (retained_result == 0 || retained_result == 1),
            "component_lifecycle_journal_corrupt", "CCLC request membership row is invalid");
  }

  Statement membership_capacity(
      database_,
      "SELECT e.owner_epoch,e.request_membership_capacity,count(m.membership_sequence) "
      "FROM cclc_owner_epochs e LEFT JOIN cclc_request_memberships m ON "
      "m.owner_epoch=e.owner_epoch GROUP BY e.owner_epoch,"
      "e.request_membership_capacity");
  while (membership_capacity.Step()) {
    const auto count =
        Nonnegative(membership_capacity.Integer(2), "owner request membership count");
    Require(count <= Positive(membership_capacity.Integer(1), "owner request-membership capacity"),
            "component_lifecycle_journal_corrupt", "owner request-membership capacity is exceeded");
  }

  std::map<std::string, bool> transaction_terminal;
  std::uint64_t retained_highest_transaction_generation = 0U;
  std::uint64_t retained_highest_terminal_sequence = 0U;
  std::size_t active_transaction_count = 0U;
  Statement transactions(database_,
                         "SELECT t.lifecycle_transaction_id,t.lifecycle_transaction_generation,"
                         "t.controllable_component_id,t.component_lifecycle_generation,"
                         "t.status_sequence,t.accepted_at_ns,t.latest_updated_at_ns,t.terminal,"
                         "t.terminal_sequence,t.status_blob,m.owner_epoch,m.request_id,"
                         "m.request_kind,m.semantic_request_digest,m.retained_result "
                         "FROM lifecycle_transactions t JOIN cclc_request_memberships m ON "
                         "m.membership_sequence=t.membership_sequence "
                         "ORDER BY t.lifecycle_transaction_generation");
  while (transactions.Step()) {
    const auto transaction_id = transactions.Text(0);
    const auto transaction_generation =
        Positive(transactions.Integer(1), "lifecycle transaction generation");
    const auto component_id = transactions.Text(2);
    const auto component_generation =
        Positive(transactions.Integer(3), "component lifecycle generation");
    const auto status_sequence = Positive(transactions.Integer(4), "lifecycle status sequence");
    const auto accepted_at_ns = transactions.Integer(5);
    const auto latest_updated_at_ns = transactions.Integer(6);
    const auto terminal = transactions.Integer(7);
    const bool terminal_sequence_absent = transactions.IsNull(8);
    const auto status = detail::DecodeComponentLifecycleStatus(transactions.Blob(9));
    const auto owner_epoch = transactions.Text(10);
    const auto request_id = transactions.Text(11);
    const auto request_kind = transactions.Text(12);
    const auto semantic_digest = transactions.Text(13);
    const auto retained_result = transactions.Integer(14);
    Require(
        (terminal == 0 || terminal == 1) && retained_result == 1 && status.locally_valid() &&
            status.acceptance.replay_status == RequestReplayStatusDTO::kAcceptedNew &&
            status.acceptance.lifecycle_transaction_id.value() == transaction_id &&
            status.acceptance.lifecycle_transaction_generation.value() == transaction_generation &&
            status.acceptance.component_lifecycle_generation.value() == component_generation &&
            status.accepted_intent.controllable_component_id.value() == component_id &&
            status.accepted_intent.request_context.request_owner_epoch.value() == owner_epoch &&
            status.accepted_intent.request_context.request_id.value() == request_id &&
            request_kind == "component_lifecycle" &&
            transaction_id == (status.apply_bring_up ? "apply-lifecycle." :
                               status.selected_bring_up ? "selected-lifecycle." : "lifecycle.") +
                                  std::to_string(transaction_generation) &&
            (status.apply_bring_up
                 ? status.apply_bring_up->original_request_payload_digest.value()
                 : status.selected_bring_up
                     ? status.selected_bring_up->original_request_payload_digest.value()
                     : detail::ComponentLifecycleSemanticIntentDigest(status.accepted_intent)) ==
                semantic_digest &&
            status.status_sequence == status_sequence && status.accepted_at_ns == accepted_at_ns &&
            status.latest_updated_at_ns == latest_updated_at_ns &&
            Terminal(status) == (terminal == 1) &&
            (terminal == 0 || TerminalStatusIsQuiescent(status)) &&
            terminal_sequence_absent == (terminal == 0),
        "component_lifecycle_journal_corrupt",
        "retained lifecycle transaction columns disagree with its status");
    if (status.apply_bring_up) {
      Statement independent(database_,
          "SELECT 1 FROM cclc_lifecycle_cell_claims WHERE lifecycle_transaction_id=? OR "
          "(owner_epoch=? AND request_id=?)");
      independent.Bind(1, transaction_id);
      independent.Bind(2, owner_epoch);
      independent.Bind(3, request_id);
      Require(!independent.Step(), "component_lifecycle_journal_corrupt",
              "delegated transaction has an independent component claim");
    }
    if (status.selected_bring_up) {
      const auto& evidence = *status.selected_bring_up;
      const auto& claim = evidence.selected_claim;
      Statement stored_claim(
          database_,
          "SELECT owner_epoch,request_id,request_payload_digest,expected_admission_generation,"
          "claim_generation,acquired_admission_generation,accepted_at_ns,program_node_evidence,"
          "recovery_configuration,lifecycle_accepted FROM cclc_lifecycle_cell_claims "
          "WHERE lifecycle_transaction_id=?");
      stored_claim.Bind(1, transaction_id);
      if (stored_claim.Step()) {
        Require(!stored_claim.IsNull(8), "component_lifecycle_journal_corrupt",
                "selected accepted claim has no full intent record");
        const auto record = detail::DecodeCCLCSelectedBringUpClaimIntent(stored_claim.Blob(8));
        Require(
            stored_claim.Text(0) == claim.intent.owner_epoch.value() &&
                stored_claim.Text(1) == claim.intent.request_id.value() &&
                stored_claim.Text(2) == evidence.offer_digest.value() &&
                stored_claim.Integer(3) ==
                    static_cast<std::int64_t>(claim.intent.expected_admission_generation.value()) &&
                !stored_claim.IsNull(4) && !stored_claim.IsNull(5) && !stored_claim.IsNull(6) &&
                stored_claim.Integer(4) ==
                    static_cast<std::int64_t>(claim.claim_generation.value()) &&
                stored_claim.Integer(5) ==
                    static_cast<std::int64_t>(claim.acquired_admission_generation.value()) &&
                stored_claim.Integer(6) == claim.accepted_at_ns && stored_claim.IsNull(7) &&
                stored_claim.Integer(9) == 1 && record.full_claim_intent == claim.intent &&
                record.original_request_payload_digest ==
                    evidence.original_request_payload_digest &&
                !stored_claim.Step(),
            "component_lifecycle_journal_corrupt",
            "selected claim/status/request identities disagree");
      } else {
        Require(Terminal(status), "component_lifecycle_journal_corrupt",
                "unsettled selected transaction has no retained claim");
      }
    }

    std::size_t projected_primitive_results = 0U;
    for (const auto& domain : status.domains) {
      projected_primitive_results += domain.primitive_attempts.size();
    }
    std::size_t completed_primitive_results = 0U;
    Statement completed_brand_effects(
        database_,
        "SELECT completed_attempt_blob FROM cclc_effect_attempts WHERE "
        "lifecycle_transaction_id=? AND "
        "effect_kind='brand_lifecycle_primitive' AND "
        "completed_attempt_blob IS NOT NULL ORDER BY effect_sequence");
    completed_brand_effects.Bind(1, transaction_id);
    while (completed_brand_effects.Step()) {
      const auto completed = detail::DecodeCCLCEffectAttempt(completed_brand_effects.Blob(0));
      Require(completed.lifecycle_primitive_result.has_value(),
              "component_lifecycle_journal_corrupt",
              "completed brand effect has no primitive result");
      const auto& result = *completed.lifecycle_primitive_result;
      std::size_t projection_matches = 0U;
      for (const auto& domain : status.domains) {
        projection_matches += static_cast<std::size_t>(
            std::count(domain.primitive_attempts.begin(), domain.primitive_attempts.end(), result));
      }
      Require(projection_matches == 1U, "component_lifecycle_journal_corrupt",
              "completed brand effect is missing or duplicated in status");
      ++completed_primitive_results;
    }
    Require(completed_primitive_results == projected_primitive_results,
            "component_lifecycle_journal_corrupt",
            "lifecycle status contains an uncompleted primitive result");

    if (terminal == 1) {
      const auto terminal_sequence =
          Positive(transactions.Integer(8), "lifecycle terminal sequence");
      retained_highest_terminal_sequence =
          std::max(retained_highest_terminal_sequence, terminal_sequence);
    } else {
      ++active_transaction_count;
    }
    const auto watermark = component_watermarks.find(component_id);
    const auto identity_guard = transaction_identity_guards.find(transaction_id);
    Require(watermark != component_watermarks.end() && watermark->second >= component_generation &&
                identity_guard != transaction_identity_guards.end() &&
                identity_guard->second == transaction_generation &&
                transaction_terminal.emplace(transaction_id, terminal == 1).second,
            "component_lifecycle_journal_corrupt",
            "retained lifecycle transaction has invalid identity or watermark");
    retained_highest_transaction_generation =
        std::max(retained_highest_transaction_generation, transaction_generation);
  }
  Require(active_transaction_count <= kMaximumCCLCActiveTransactions &&
              retained_highest_transaction_generation <= highest_transaction_generation &&
              retained_highest_terminal_sequence <= highest_terminal_sequence,
          "component_lifecycle_journal_corrupt",
          "lifecycle transaction capacity or durable watermark is invalid");

  std::map<std::string, std::uint64_t> effect_sequence_watermarks;
  std::map<std::string, std::uint64_t> effect_history_bytes;
  std::set<std::string> transactions_with_unresolved_effects;
  Statement effects(database_,
                    "SELECT a.lifecycle_transaction_id,t.lifecycle_transaction_generation,"
                    "a.effect_sequence,a.effect_kind,a.effect_identity,"
                    "a.prepared_status_sequence,a.prepared_status_digest,"
                    "a.prepared_attempt_blob,a.completed_status_sequence,"
                    "a.completed_status_digest,a.completed_attempt_blob "
                    "FROM cclc_effect_attempts a JOIN lifecycle_transactions t ON "
                    "t.lifecycle_transaction_id=a.lifecycle_transaction_id "
                    "ORDER BY a.lifecycle_transaction_id,a.effect_sequence");
  while (effects.Step()) {
    const auto transaction_id = effects.Text(0);
    const auto transaction_generation =
        Positive(effects.Integer(1), "effect transaction generation");
    const auto effect_sequence = Positive(effects.Integer(2), "CCLC effect sequence");
    const auto effect_kind = effects.Text(3);
    const auto effect_identity = effects.Text(4);
    const auto prepared_status_sequence =
        Positive(effects.Integer(5), "prepared effect status sequence");
    const auto prepared_status_digest = effects.Text(6);
    const auto prepared_blob = effects.Blob(7);
    const auto prepared = detail::DecodeCCLCEffectAttempt(prepared_blob);
    auto& highest_effect_sequence = effect_sequence_watermarks[transaction_id];
    Require(effect_sequence == highest_effect_sequence + 1U && prepared.locally_valid() &&
                prepared.state == CCLCEffectAttemptStateDTO::kPrepared &&
                prepared.lifecycle_transaction_id.value() == transaction_id &&
                prepared.lifecycle_transaction_generation.value() == transaction_generation &&
                prepared.effect_sequence == effect_sequence &&
                EffectKindName(prepared.effect_kind) == effect_kind &&
                EffectIdentity(prepared) == effect_identity &&
                content_identity::is_sha256_digest(prepared_status_digest) &&
                prepared_status_sequence > 0U,
            "component_lifecycle_journal_corrupt",
            "prepared CCLC effect row is invalid or noncontiguous");
    highest_effect_sequence = effect_sequence;
    auto& history_bytes = effect_history_bytes[transaction_id];
    history_bytes += prepared_blob.size();
    if (effects.IsNull(10)) {
      Require(effects.IsNull(8) && effects.IsNull(9), "component_lifecycle_journal_corrupt",
              "unresolved CCLC effect has completed columns");
      transactions_with_unresolved_effects.insert(transaction_id);
    } else {
      Require(!effects.IsNull(8) && !effects.IsNull(9), "component_lifecycle_journal_corrupt",
              "completed CCLC effect is missing completed columns");
      const auto completed_status_sequence =
          Positive(effects.Integer(8), "completed effect status sequence");
      const auto completed_status_digest = effects.Text(9);
      const auto completed_blob = effects.Blob(10);
      const auto completed = detail::DecodeCCLCEffectAttempt(completed_blob);
      Require(completed.locally_valid() &&
                  completed.state == CCLCEffectAttemptStateDTO::kCompleted &&
                  PreparedForm(completed) == prepared &&
                  completed_status_sequence > prepared_status_sequence &&
                  content_identity::is_sha256_digest(completed_status_digest),
              "component_lifecycle_journal_corrupt",
              "completed CCLC effect row conflicts with its preparation");
      history_bytes += completed_blob.size();
    }
    Require(history_bytes <= detail::kMaximumComponentLifecycleJournalRecordBytes,
            "component_lifecycle_journal_corrupt",
            "CCLC effect history exceeds its logical byte bound");
  }
  for (const auto& transaction_id : transactions_with_unresolved_effects) {
    const auto terminal = transaction_terminal.find(transaction_id);
    Require(terminal != transaction_terminal.end() && !terminal->second,
            "component_lifecycle_journal_corrupt",
            "terminal lifecycle transaction has an unresolved effect");
  }

  std::map<std::string, bool> route_transaction_terminal;
  std::uint64_t retained_highest_route_generation = 0U;
  Statement route_transactions(
      database_,
      "SELECT t.route_selection_transaction_id,"
      "t.route_selection_transaction_generation,t.control_domain_id,"
      "t.active_control_configuration_generation,t.status_sequence,"
      "t.accepted_at_ns,t.latest_updated_at_ns,t.terminal,t.terminal_sequence,"
      "t.status_blob,m.owner_epoch,m.request_id,m.request_kind,"
      "m.semantic_request_digest,m.retained_result "
      "FROM route_selection_transactions t JOIN cclc_request_memberships m ON "
      "m.membership_sequence=t.membership_sequence "
      "ORDER BY t.route_selection_transaction_generation");
  while (route_transactions.Step()) {
    const auto transaction_id = route_transactions.Text(0);
    const auto transaction_generation =
        Positive(route_transactions.Integer(1), "route-selection transaction generation");
    const auto control_domain_id = route_transactions.Text(2);
    const auto active_configuration_generation =
        Positive(route_transactions.Integer(3), "active-control-configuration generation");
    const auto status_sequence =
        Positive(route_transactions.Integer(4), "route-selection status sequence");
    const auto accepted_at_ns = route_transactions.Integer(5);
    const auto latest_updated_at_ns = route_transactions.Integer(6);
    const auto terminal = route_transactions.Integer(7);
    const bool terminal_sequence_absent = route_transactions.IsNull(8);
    const auto status = detail::DecodeRouteSelectionStatus(route_transactions.Blob(9));
    const auto owner_epoch = route_transactions.Text(10);
    const auto request_id = route_transactions.Text(11);
    const auto request_kind = route_transactions.Text(12);
    const auto semantic_digest = route_transactions.Text(13);
    const auto retained_result = route_transactions.Integer(14);
    Require(
        (terminal == 0 || terminal == 1) && retained_result == 1 && status.locally_valid() &&
            status.acceptance.replay_status == RequestReplayStatusDTO::kAcceptedNew &&
            status.acceptance.route_selection_transaction_id.value() == transaction_id &&
            status.acceptance.route_selection_transaction_generation.value() ==
                transaction_generation &&
            status.acceptance.desired_configuration.control_domain_id.value() ==
                control_domain_id &&
            status.acceptance.candidate_active_control_configuration_generation.value() ==
                active_configuration_generation &&
            status.accepted_intent.request_context.request_owner_epoch.value() == owner_epoch &&
            status.accepted_intent.request_context.request_id.value() == request_id &&
            request_kind == "route_selection" &&
            detail::RouteSelectionSemanticIntentDigest(status.accepted_intent) == semantic_digest &&
            status.status_sequence == status_sequence && status.accepted_at_ns == accepted_at_ns &&
            status.latest_updated_at_ns == latest_updated_at_ns &&
            Terminal(status) == (terminal == 1) && terminal_sequence_absent == (terminal == 0),
        "component_lifecycle_journal_corrupt",
        "retained route-selection columns disagree with its status");

    if (terminal == 1) {
      retained_highest_terminal_sequence =
          std::max(retained_highest_terminal_sequence,
                   Positive(route_transactions.Integer(8), "route terminal sequence"));
    }
    const auto watermark = active_configuration_watermarks.find(control_domain_id);
    const auto identity_guard = route_transaction_identity_guards.find(transaction_id);
    Require(watermark != active_configuration_watermarks.end() &&
                watermark->second >= active_configuration_generation &&
                identity_guard != route_transaction_identity_guards.end() &&
                identity_guard->second == transaction_generation &&
                route_transaction_terminal.emplace(transaction_id, terminal == 1).second,
            "component_lifecycle_journal_corrupt",
            "retained route-selection transaction has invalid identity or watermark");
    retained_highest_route_generation =
        std::max(retained_highest_route_generation, transaction_generation);

    std::vector<ControllerLifecycleTransitionResultDTO> completed_controller_results;
    std::optional<RouteConfigurationFenceAcceptedDTO> completed_fence;
    std::optional<RouteConfigurationFenceReleasedDTO> completed_release;
    Statement completed_route_effects(
        database_,
        "SELECT completed_attempt_blob FROM cclc_route_effect_attempts WHERE "
        "route_selection_transaction_id=? AND completed_attempt_blob IS NOT NULL "
        "ORDER BY effect_sequence");
    completed_route_effects.Bind(1, transaction_id);
    while (completed_route_effects.Step()) {
      const auto completed = detail::DecodeCCLCRouteEffectAttempt(completed_route_effects.Blob(0));
      switch (completed.effect_kind) {
        case CCLCRouteEffectKindDTO::kKernelRouteConfigurationFenceAcquire: {
          const auto& result =
              std::get<RouteConfigurationFenceAcquireEffectResultDTO>(*completed.result);
          if (result.disposition == RouteConfigurationFenceAcquireEffectDispositionDTO::kAccepted) {
            Require(!completed_fence.has_value() && result.acceptance.has_value(),
                    "component_lifecycle_journal_corrupt",
                    "route has duplicate accepted fence effects");
            completed_fence = result.acceptance;
          }
          break;
        }
        case CCLCRouteEffectKindDTO::kControllerLifecycleTransition:
          completed_controller_results.push_back(
              std::get<ControllerLifecycleTransitionResultDTO>(*completed.result));
          break;
        case CCLCRouteEffectKindDTO::kKernelRouteConfigurationFenceRelease: {
          const auto& result =
              std::get<RouteConfigurationFenceReleaseEffectResultDTO>(*completed.result);
          if (result.disposition == RouteConfigurationFenceReleaseEffectDispositionDTO::kReleased) {
            Require(!completed_release.has_value() && result.receipt.has_value(),
                    "component_lifecycle_journal_corrupt",
                    "route has duplicate released fence effects");
            completed_release = result.receipt;
          }
          break;
        }
      }
    }
    Require(completed_controller_results == status.controller_transition_results &&
                completed_fence == status.accepted_fence &&
                completed_release == status.released_fence,
            "component_lifecycle_journal_corrupt",
            "route status disagrees with its completed effect projection");
  }
  Require(retained_highest_route_generation <= highest_route_selection_transaction_generation &&
              ActiveTransactionCount() <= kMaximumCCLCActiveTransactions &&
              retained_highest_terminal_sequence <= highest_terminal_sequence,
          "component_lifecycle_journal_corrupt",
          "route transaction capacity or durable watermark is invalid");

  std::map<std::string, std::uint64_t> route_effect_sequence_watermarks;
  std::map<std::string, std::uint64_t> route_effect_history_bytes;
  std::set<std::string> route_transactions_with_unresolved_effects;
  Statement route_effects(
      database_,
      "SELECT a.route_selection_transaction_id,"
      "t.route_selection_transaction_generation,a.effect_sequence,a.effect_kind,"
      "a.effect_identity,a.prepared_status_sequence,a.prepared_status_digest,"
      "a.prepared_attempt_blob,a.completed_status_sequence,"
      "a.completed_status_digest,a.completed_attempt_blob "
      "FROM cclc_route_effect_attempts a JOIN route_selection_transactions t ON "
      "t.route_selection_transaction_id=a.route_selection_transaction_id "
      "ORDER BY a.route_selection_transaction_id,a.effect_sequence");
  while (route_effects.Step()) {
    const auto transaction_id = route_effects.Text(0);
    const auto transaction_generation =
        Positive(route_effects.Integer(1), "route effect transaction generation");
    const auto effect_sequence = Positive(route_effects.Integer(2), "route effect sequence");
    const auto effect_kind = route_effects.Text(3);
    const auto effect_identity = route_effects.Text(4);
    const auto prepared_status_sequence =
        Positive(route_effects.Integer(5), "prepared route effect status sequence");
    const auto prepared_status_digest = route_effects.Text(6);
    const auto prepared_blob = route_effects.Blob(7);
    const auto prepared = detail::DecodeCCLCRouteEffectAttempt(prepared_blob);
    auto& highest_effect_sequence = route_effect_sequence_watermarks[transaction_id];
    Require(effect_sequence == highest_effect_sequence + 1U && prepared.locally_valid() &&
                prepared.state == CCLCRouteEffectAttemptStateDTO::kPrepared &&
                prepared.route_selection_transaction_id.value() == transaction_id &&
                prepared.route_selection_transaction_generation.value() == transaction_generation &&
                prepared.effect_sequence == effect_sequence &&
                RouteEffectKindName(prepared.effect_kind) == effect_kind &&
                RouteEffectIdentity(prepared) == effect_identity &&
                content_identity::is_sha256_digest(prepared_status_digest) &&
                prepared_status_sequence > 0U,
            "component_lifecycle_journal_corrupt",
            "prepared route effect row is invalid or noncontiguous");
    highest_effect_sequence = effect_sequence;
    auto& history_bytes = route_effect_history_bytes[transaction_id];
    history_bytes += prepared_blob.size();
    if (route_effects.IsNull(10)) {
      Require(route_effects.IsNull(8) && route_effects.IsNull(9),
              "component_lifecycle_journal_corrupt",
              "unresolved route effect has completed columns");
      route_transactions_with_unresolved_effects.insert(transaction_id);
    } else {
      Require(!route_effects.IsNull(8) && !route_effects.IsNull(9),
              "component_lifecycle_journal_corrupt",
              "completed route effect is missing completed columns");
      const auto completed_status_sequence =
          Positive(route_effects.Integer(8), "completed route effect status sequence");
      const auto completed_status_digest = route_effects.Text(9);
      const auto completed_blob = route_effects.Blob(10);
      const auto completed = detail::DecodeCCLCRouteEffectAttempt(completed_blob);
      Require(completed.locally_valid() &&
                  completed.state == CCLCRouteEffectAttemptStateDTO::kCompleted &&
                  PreparedForm(completed) == prepared &&
                  completed_status_sequence > prepared_status_sequence &&
                  content_identity::is_sha256_digest(completed_status_digest),
              "component_lifecycle_journal_corrupt",
              "completed route effect conflicts with its preparation");
      history_bytes += completed_blob.size();
    }
    Require(history_bytes <= detail::kMaximumComponentLifecycleJournalRecordBytes,
            "component_lifecycle_journal_corrupt",
            "route effect history exceeds its logical byte bound");
  }
  for (const auto& transaction_id : route_transactions_with_unresolved_effects) {
    const auto terminal = route_transaction_terminal.find(transaction_id);
    Require(terminal != route_transaction_terminal.end() && !terminal->second,
            "component_lifecycle_journal_corrupt",
            "terminal route transaction has an unresolved effect");
  }

  // The optional Q97 link is immutable data even after the original owner
  // disappears. Validate its closed header/component joins without deriving authority.
  Statement linked_native(database_, "SELECT apply_id,apply_generation FROM cclc_native_apply_plans "
                                      "WHERE component_bring_up IS NOT NULL");
  if (linked_native.Step()) {
    const NamedApplyPublicationTransactionDTO apply{
        ApplyId(linked_native.Text(0)),
        ApplyGeneration(Positive(linked_native.Integer(1), "linked native Apply generation"))};
    Require(!linked_native.Step() && ReadNativeApplyPlanLocked(apply).has_value(),
            "component_lifecycle_journal_corrupt", "linked native Apply plan is invalid or duplicated");
  }
  Statement terminal_accounting(database_,
                                "SELECT count(*),coalesce(sum(retained_bytes),0) FROM (" +
                                    std::string(kSettledResults) + ")");
  Require(terminal_accounting.Step() &&
              Nonnegative(terminal_accounting.Integer(0), "terminal result count") <=
                  exact_result_history_capacity_ &&
              Nonnegative(terminal_accounting.Integer(1), "terminal result bytes") <=
                  terminal_result_bytes_capacity_ &&
              !terminal_accounting.Step(),
          "component_lifecycle_journal_corrupt", "retained terminal result bounds are exceeded");
}

void SqliteCCLCJournal::Impl::ReadQuiescentOwner(const RequestOwnerEpoch& expected) {
  Require(!expected.empty() && expected.value().size() <= kMaxLifecycleIdentityBytes,
          "component_lifecycle_journal_query_invalid", "expected lifecycle owner epoch is invalid");
  std::lock_guard lock(mutex_);
  RequireOpen();
  ReadTransaction transaction(database_);
  const auto quiescence = ReadOwnerQuiescenceLocked(expected);
  Require(quiescence != OwnerQuiescence::kOwnerMismatch,
          "component_lifecycle_journal_owner_epoch_stale",
          "quiescence observation requires the exact active lifecycle owner");
  Require(quiescence == OwnerQuiescence::kQuiescent,
          "component_lifecycle_checkpoint_not_quiescent",
          "the lifecycle owner retains unsettled transactions, claims or effect attempts");
  transaction.Commit();
}

CCLCJournalRecoveryDTO SqliteCCLCJournal::Impl::BeginOwnerEpoch(
    const RequestOwnerEpoch& unique_seed, std::int64_t started_at_ns,
    std::size_t request_membership_capacity,
    const std::optional<RequestOwnerEpoch>& quiescent_predecessor,
    CCLCOwnerEpochTransitionPurpose purpose) {
  Require((purpose == CCLCOwnerEpochTransitionPurpose::kRestart ||
           purpose == CCLCOwnerEpochTransitionPurpose::kLiveCapacityCheckpoint) &&
              (purpose != CCLCOwnerEpochTransitionPurpose::kLiveCapacityCheckpoint ||
               quiescent_predecessor.has_value()),
          "component_lifecycle_journal_owner_epoch_invalid",
          "owner transition purpose or checkpoint predecessor is invalid");
  Require(!unique_seed.empty() && unique_seed.value().size() <= kMaxLifecycleIdentityBytes &&
              started_at_ns >= 0 && request_membership_capacity > 0U &&
              request_membership_capacity <= kMaximumCCLCRequestMembership &&
              exact_result_history_capacity_ <= request_membership_capacity,
          "component_lifecycle_journal_owner_epoch_invalid",
          "lifecycle owner epoch or request-membership bound is invalid");
  std::lock_guard lock(mutex_);
  RequireOpen();
  Transaction transaction(database_);

  if (quiescent_predecessor) {
    Require(
        ReadOwnerQuiescenceLocked(*quiescent_predecessor) == OwnerQuiescence::kQuiescent,
        "component_lifecycle_checkpoint_not_quiescent",
        "request namespace checkpoint requires the exact settled predecessor");
  }

  const auto sequence = OwnerSequence();
  Require(sequence < 9'007'199'254'740'991ULL, "component_lifecycle_owner_epoch_exhausted",
          "request namespace counter cannot advance without wrapping");
  using content_identity::CanonicalValue;
  const RequestOwnerEpoch owner_epoch(content_identity::sha256_digest(
      content_identity::canonical_json(CanonicalValue(CanonicalValue::Object{
          {"domain", CanonicalValue("manyforge.cclc.request-namespace.v1")},
          {"cell_id", CanonicalValue(cell_id_.value())},
          {"unique_seed", CanonicalValue(unique_seed.value())},
          {"epoch_sequence", CanonicalValue(std::to_string(sequence + 1U))}}))));

  Exec(database_, "UPDATE cclc_owner_epochs SET active=0 WHERE active=1");
  Statement insert(database_,
                   "INSERT INTO cclc_owner_epochs(owner_epoch,started_at_ns,"
                   "request_membership_capacity,active) VALUES(?,?,?,1)");
  insert.Bind(1, owner_epoch.value());
  insert.Bind(2, started_at_ns);
  insert.Bind(3, static_cast<std::int64_t>(request_membership_capacity));
  Require(!insert.Step(), "component_lifecycle_journal_corrupt",
          "lifecycle owner epoch insert returned an unexpected row");
  Require(sqlite3_last_insert_rowid(database_) == static_cast<std::int64_t>(sequence + 1U),
          "component_lifecycle_journal_corrupt", "request namespace counter is not exact");

  RetireQuiescentRequestNamespaces();

  // Only the trusted live capacity checkpoint preserves Active intent.
  // Startup retires it even when an exact settled predecessor is supplied.
  // Retirement is not an inactive-hardware observation or Recover/Reset.
  if (purpose == CCLCOwnerEpochTransitionPurpose::kRestart) {
    std::vector<ControllableComponentId> active_targets;
    {
      Statement targets(database_,
                        "SELECT controllable_component_id FROM cclc_component_lifecycle_targets "
                        "WHERE present=1 AND requested_state=? ORDER BY controllable_component_id");
      targets.Bind(1, static_cast<std::int64_t>(LifecycleTargetStateDTO::kActive));
      while (targets.Step()) active_targets.emplace_back(targets.Text(0));
    }
    for (const auto& component : active_targets) RetireLifecycleTargetIntentLocked(component);
  }

  CCLCJournalRecoveryDTO recovery{
      Nonnegative(ScalarInteger(database_,
                                "SELECT highest_transaction_generation FROM "
                                "cclc_journal_metadata WHERE singleton=1"),
                  "highest lifecycle transaction generation"),
      {},
      {},
      {},
      {},
      Nonnegative(ScalarInteger(database_,
                                "SELECT highest_route_selection_transaction_generation FROM "
                                "cclc_journal_metadata WHERE singleton=1"),
                  "highest route-selection transaction generation"),
      {},
      {},
      {},
      {},
      {},
      {}};
  recovery.request_owner_epoch = owner_epoch;

  Statement watermarks(database_,
                       "SELECT controllable_component_id,component_lifecycle_generation "
                       "FROM lifecycle_component_watermarks ORDER BY controllable_component_id");
  while (watermarks.Step()) {
    recovery.component_watermarks.push_back(ComponentLifecycleGenerationWatermarkDTO{
        ControllableComponentId(watermarks.Text(0)),
        ComponentLifecycleGeneration(
            Positive(watermarks.Integer(1), "component lifecycle watermark"))});
  }

  Statement interrupted(database_,
                        "SELECT status_blob FROM lifecycle_transactions WHERE terminal=0 "
                        "ORDER BY lifecycle_transaction_generation");
  while (interrupted.Step()) {
    recovery.interrupted_transactions.push_back(
        detail::DecodeComponentLifecycleStatus(interrupted.Blob(0)));
  }
  Statement effect_watermarks(database_,
                              "SELECT t.lifecycle_transaction_id,"
                              "t.lifecycle_transaction_generation,"
                              "coalesce(max(a.effect_sequence),0) "
                              "FROM lifecycle_transactions t LEFT JOIN cclc_effect_attempts a ON "
                              "a.lifecycle_transaction_id=t.lifecycle_transaction_id "
                              "WHERE t.terminal=0 GROUP BY t.lifecycle_transaction_id,"
                              "t.lifecycle_transaction_generation "
                              "ORDER BY t.lifecycle_transaction_generation");
  while (effect_watermarks.Step()) {
    recovery.effect_sequence_watermarks.push_back(CCLCTransactionEffectSequenceWatermarkDTO{
        LifecycleTransactionId(effect_watermarks.Text(0)),
        LifecycleTransactionGeneration(
            Positive(effect_watermarks.Integer(1), "lifecycle transaction generation")),
        Nonnegative(effect_watermarks.Integer(2), "highest CCLC effect sequence")});
  }
  Statement attempts(database_,
                     "SELECT a.prepared_attempt_blob FROM cclc_effect_attempts a "
                     "JOIN lifecycle_transactions t ON t.lifecycle_transaction_id="
                     "a.lifecycle_transaction_id WHERE t.terminal=0 AND "
                     "a.completed_attempt_blob IS NULL "
                     "ORDER BY t.lifecycle_transaction_generation,"
                     "a.effect_sequence");
  while (attempts.Step()) {
    recovery.unresolved_effect_attempts.push_back(
        detail::DecodeCCLCEffectAttempt(attempts.Blob(0)));
  }

  Statement active_configuration_watermarks(
      database_,
      "SELECT control_domain_id,active_control_configuration_generation "
      "FROM control_domain_active_configuration_watermarks ORDER BY control_domain_id");
  while (active_configuration_watermarks.Step()) {
    recovery.active_configuration_watermarks.push_back(
        ControlDomainActiveConfigurationGenerationWatermarkDTO{
            ControlDomainId(active_configuration_watermarks.Text(0)),
            ActiveControlConfigurationGeneration(
                Positive(active_configuration_watermarks.Integer(1),
                         "active-control-configuration watermark"))});
  }

  Statement current_active_configurations(
      database_,
      "SELECT configuration_blob FROM control_domain_active_configurations "
      "ORDER BY control_domain_id");
  while (current_active_configurations.Step()) {
    recovery.current_active_control_configurations.push_back(
        detail::DecodeActiveControlConfiguration(current_active_configurations.Blob(0)));
  }

  Statement interrupted_routes(
      database_,
      "SELECT status_blob FROM route_selection_transactions WHERE terminal=0 "
      "ORDER BY route_selection_transaction_generation");
  while (interrupted_routes.Step()) {
    recovery.interrupted_route_selections.push_back(
        detail::DecodeRouteSelectionStatus(interrupted_routes.Blob(0)));
  }
  Statement route_effect_watermarks(
      database_,
      "SELECT t.route_selection_transaction_id,"
      "t.route_selection_transaction_generation,coalesce(max(a.effect_sequence),0) "
      "FROM route_selection_transactions t LEFT JOIN cclc_route_effect_attempts a ON "
      "a.route_selection_transaction_id=t.route_selection_transaction_id "
      "WHERE t.terminal=0 GROUP BY t.route_selection_transaction_id,"
      "t.route_selection_transaction_generation "
      "ORDER BY t.route_selection_transaction_generation");
  while (route_effect_watermarks.Step()) {
    recovery.route_effect_sequence_watermarks.push_back(CCLCRouteEffectSequenceWatermarkDTO{
        RouteSelectionTransactionId(route_effect_watermarks.Text(0)),
        RouteSelectionTransactionGeneration(
            Positive(route_effect_watermarks.Integer(1), "route-selection transaction generation")),
        Nonnegative(route_effect_watermarks.Integer(2), "highest route effect sequence")});
  }
  Statement route_attempts(
      database_,
      "SELECT a.prepared_attempt_blob FROM cclc_route_effect_attempts a "
      "JOIN route_selection_transactions t ON t.route_selection_transaction_id="
      "a.route_selection_transaction_id WHERE t.terminal=0 AND "
      "a.completed_attempt_blob IS NULL "
      "ORDER BY t.route_selection_transaction_generation,a.effect_sequence");
  while (route_attempts.Step()) {
    recovery.unresolved_route_effect_attempts.push_back(
        detail::DecodeCCLCRouteEffectAttempt(route_attempts.Blob(0)));
  }
  Statement cell_claims(database_,
                        "SELECT lifecycle_transaction_id,owner_epoch,request_id,"
                        "request_payload_digest,expected_admission_generation,claim_generation,"
                        "acquired_admission_generation,accepted_at_ns,release_kind,"
                        "release_evidence_digest,release_receipt_recorded,lifecycle_accepted,"
                        "program_node_evidence,recovery_configuration "
                        "FROM cclc_lifecycle_cell_claims WHERE release_receipt_recorded=0 "
                        "ORDER BY rowid");
  while (cell_claims.Step()) {
    CellAdmissionClaimIntentDTO intent{
        CellAdmissionClaimKindDTO::kComponentLifecycle,
        CellAdmissionOwnerRoleDTO::kComponentLifecycleOwner,
        RequestOwnerEpoch(cell_claims.Text(1)),
        cell_claims.Text(0),
        RequestId(cell_claims.Text(2)),
        ContentDigest(cell_claims.Text(3)),
        AdmissionGeneration(Positive(cell_claims.Integer(4), "claim admission generation")),
        {},
        std::nullopt,
        std::nullopt};
    if (!cell_claims.IsNull(12)) {
      intent.program_node_evidence =
          detail::DecodeProgramNodeAdmissionEvidence(cell_claims.Blob(12));
    }
    std::optional<ContentDigest> original_request_digest;
    if (!cell_claims.IsNull(13))
      original_request_digest = RestoreRouteRecoveryClaim(intent, cell_claims.Blob(13));
    Require(!intent.owner_transaction_id.starts_with("selected-lifecycle.") ||
                original_request_digest.has_value(),
            "component_lifecycle_journal_corrupt",
            "selected claim has no original request identity");
    Require(ValidCCLCClaimIntent(intent), "component_lifecycle_journal_corrupt",
            "recovered CCLC claim is invalid");
    std::optional<CellAdmissionClaimDTO> claim;
    if (!cell_claims.IsNull(5)) {
      claim = CellAdmissionClaimDTO{
          intent,
          CellAdmissionClaimGeneration(Positive(cell_claims.Integer(5), "claim generation")),
          AdmissionGeneration(Positive(cell_claims.Integer(6), "acquired admission generation")),
          cell_claims.Integer(7)};
    }
    std::optional<CellAdmissionClaimReleaseDTO> release;
    if (!cell_claims.IsNull(8)) {
      Require(claim.has_value(), "component_lifecycle_journal_corrupt",
              "lifecycle release intent has no claim");
      release = CellAdmissionClaimReleaseDTO{
          intent.owner_role,
          intent.owner_epoch,
          intent.owner_transaction_id,
          intent.request_id,
          intent.request_payload_digest,
          claim->acquired_admission_generation,
          claim->claim_generation,
          {static_cast<CellAdmissionSettlementKindDTO>(cell_claims.Integer(8)),
           ContentDigest(cell_claims.Text(9))}};
    }
    recovery.cell_claims.push_back({std::move(intent), std::move(claim), std::move(release),
                                    cell_claims.Integer(10) == 1, cell_claims.Integer(11) == 1,
                                    std::move(original_request_digest)});
  }
  Require(recovery.locally_valid(), "component_lifecycle_journal_corrupt",
          "lifecycle journal recovery projection is invalid");
  transaction.Commit();
  return recovery;
}

std::optional<CCLCLifecycleRequestResultDTO> SqliteCCLCJournal::Impl::InspectLifecycleRequestLocked(
    const RequestOwnerEpoch& active_owner_epoch, const ComponentLifecycleIntentDTO& intent,
    std::string_view semantic_digest) {
  if (intent.request_context.request_owner_epoch != active_owner_epoch ||
      !ActiveOwner(active_owner_epoch)) {
    return CCLCLifecycleRequestResultDTO{RequestReplayStatusDTO::kStaleRequestOwnerEpoch,
                                         std::nullopt, std::nullopt};
  }

  if (auto retained = FindLifecycleRequestResultLocked(intent, semantic_digest)) return retained;
  if (MembershipFull(active_owner_epoch)) {
    return CCLCLifecycleRequestResultDTO{RequestReplayStatusDTO::kRequestLedgerFull, std::nullopt,
                                         std::nullopt};
  }
  return std::nullopt;
}

std::optional<CCLCLifecycleRequestResultDTO>
SqliteCCLCJournal::Impl::FindLifecycleRequestResultLocked(const ComponentLifecycleIntentDTO& intent,
                                                          std::string_view semantic_digest) {
  Statement membership_lookup(database_,
                              "SELECT membership_sequence,request_kind,semantic_request_digest,"
                              "retained_result,rejection_blob FROM cclc_request_memberships "
                              "WHERE owner_epoch=? AND request_id=?");
  membership_lookup.Bind(1, intent.request_context.request_owner_epoch.value());
  membership_lookup.Bind(2, intent.request_context.request_id.value());
  if (!membership_lookup.Step()) {
    return std::nullopt;
  }
  const auto membership_sequence =
      Positive(membership_lookup.Integer(0), "request membership sequence");
  const auto request_kind = membership_lookup.Text(1);
  const auto retained_digest = membership_lookup.Text(2);
  const auto retained_result = membership_lookup.Integer(3);
  const auto rejection = membership_lookup.IsNull(4)
                             ? std::optional<ErrorDTO>{}
                             : detail::DecodeCCLCAdmissionRejection(membership_lookup.Blob(4));
  Require((retained_result == 0 || retained_result == 1) && !membership_lookup.Step(),
          "component_lifecycle_journal_corrupt",
          "CCLC request membership is invalid or duplicated");
  if (request_kind != "component_lifecycle" || retained_digest != semantic_digest) {
    return CCLCLifecycleRequestResultDTO{RequestReplayStatusDTO::kRequestIdConflict, std::nullopt,
                                         std::nullopt};
  }
  if (retained_result == 0) {
    return CCLCLifecycleRequestResultDTO{RequestReplayStatusDTO::kRequestResultExpired,
                                         std::nullopt, std::nullopt};
  }
  if (rejection) {
    return CCLCLifecycleRequestResultDTO{RequestReplayStatusDTO::kReplayedExact, std::nullopt,
                                         std::nullopt, rejection};
  }
  Statement retained(database_,
                     "SELECT status_blob FROM lifecycle_transactions "
                     "WHERE membership_sequence=?");
  retained.Bind(1, static_cast<std::int64_t>(membership_sequence));
  if (!retained.Step()) {
    // The other accepted form: the operator's Recover that closed an incident
    // (authorization only, no transaction).
    auto recovery = ReadComponentIncidentRecoveryLocked(intent.request_context);
    Require(recovery.has_value() &&
                recovery->controllable_component_id == intent.controllable_component_id,
            "component_lifecycle_journal_corrupt",
            "retained lifecycle request has no transaction or incident recovery result");
    CCLCLifecycleRequestResultDTO result{RequestReplayStatusDTO::kReplayedExact, std::nullopt,
                                         std::nullopt};
    result.incident_recovery = std::move(*recovery);
    return result;
  }
  auto status = detail::DecodeComponentLifecycleStatus(retained.Blob(0));
  Require(!retained.Step(), "component_lifecycle_journal_corrupt",
          "retained lifecycle request has duplicate transaction results");
  auto acceptance = status.acceptance;
  acceptance.replay_status = RequestReplayStatusDTO::kReplayedExact;
  return CCLCLifecycleRequestResultDTO{RequestReplayStatusDTO::kReplayedExact,
                                       std::move(acceptance), std::move(status)};
}

std::optional<CCLCLifecycleRequestResultDTO> SqliteCCLCJournal::Impl::InspectLifecycleRequest(
    const RequestOwnerEpoch& active_owner_epoch, const ComponentLifecycleIntentDTO& intent,
    const ContentDigest* original_request_digest, bool apply_delegated) {
  Require(!active_owner_epoch.empty() && intent.locally_valid(),
          "component_lifecycle_journal_record_invalid",
          "lifecycle request inspection input is invalid");
  Require(
      !original_request_digest || IsExactLowercaseSha256Digest(original_request_digest->value()),
      "component_lifecycle_journal_query_invalid", "selected offer digest is invalid");
  const auto semantic_digest = original_request_digest
                                   ? original_request_digest->value()
                                   : detail::ComponentLifecycleSemanticIntentDigest(intent);
  std::lock_guard lock(mutex_);
  RequireOpen();
  Transaction transaction(database_);
  auto result = InspectLifecycleRequestLocked(active_owner_epoch, intent, semantic_digest);
  if (result && result->status &&
      (result->status->apply_bring_up.has_value() != apply_delegated ||
       result->status->selected_bring_up.has_value() !=
           (original_request_digest != nullptr && !apply_delegated) ||
       (original_request_digest &&
        (!result->status->accepted_intent.replay_equal(intent) ||
         (apply_delegated ? result->status->apply_bring_up->original_request_payload_digest
                          : result->status->selected_bring_up->original_request_payload_digest) !=
             *original_request_digest))))
    result = CCLCLifecycleRequestResultDTO{RequestReplayStatusDTO::kRequestIdConflict, std::nullopt,
                                           std::nullopt};
  transaction.Commit();
  return result;
}

std::optional<CCLCLifecycleRequestResultDTO> SqliteCCLCJournal::Impl::ReadLifecycleRequestResult(
    const ComponentLifecycleIntentDTO& intent, const ContentDigest* original_request_digest, bool apply_delegated) {
  Require(intent.locally_valid(), "component_lifecycle_journal_query_invalid",
          "lifecycle result query is invalid");
  Require(
      !original_request_digest || IsExactLowercaseSha256Digest(original_request_digest->value()),
      "component_lifecycle_journal_query_invalid", "selected offer digest is invalid");
  const auto digest = original_request_digest
                          ? original_request_digest->value()
                          : detail::ComponentLifecycleSemanticIntentDigest(intent);
  std::lock_guard lock(mutex_);
  RequireOpen();
  ReadTransaction transaction(database_);
  auto result = FindLifecycleRequestResultLocked(intent, digest);
  if (!result && UnstartedInFullNamespace(intent.request_context)) {
    // Pure absence proof in a complete immutable membership set. The owner
    // maps this to explicit unstarted backpressure, never to acceptance.
    result = CCLCLifecycleRequestResultDTO{RequestReplayStatusDTO::kRequestLedgerFull, std::nullopt,
                                           std::nullopt};
  }
  if (result && result->status &&
      (result->status->apply_bring_up.has_value() != apply_delegated ||
       result->status->selected_bring_up.has_value() !=
           (original_request_digest != nullptr && !apply_delegated) ||
       (original_request_digest &&
        (!result->status->accepted_intent.replay_equal(intent) ||
         (apply_delegated ? result->status->apply_bring_up->original_request_payload_digest
                          : result->status->selected_bring_up->original_request_payload_digest) !=
             *original_request_digest))))
    result = CCLCLifecycleRequestResultDTO{RequestReplayStatusDTO::kRequestIdConflict, std::nullopt,
                                           std::nullopt};
  transaction.Commit();
  return result;
}

CCLCLifecycleRequestResultDTO SqliteCCLCJournal::Impl::RecordLifecycleRejection(
    const RequestOwnerEpoch& owner, const ComponentLifecycleIntentDTO& intent,
    const ErrorDTO& error, const std::optional<CellAdmissionClaimIntentDTO>& unacquired,
    const ContentDigest* original_request_digest) {
  Require(
      intent.locally_valid() && owner == intent.request_context.request_owner_epoch &&
          (!original_request_digest ||
           (intent.intent == ComponentLifecycleIntentKindDTO::kEnable &&
            !intent.program_node_evidence &&
            IsExactLowercaseSha256Digest(original_request_digest->value()) &&
            (!unacquired || unacquired->owner_transaction_id.starts_with("selected-lifecycle.")))),
      "component_lifecycle_journal_record_invalid", "no-admission result has invalid identity");
  const auto digest = original_request_digest
                          ? original_request_digest->value()
                          : detail::ComponentLifecycleSemanticIntentDigest(intent);
  std::lock_guard lock(mutex_);
  RequireOpen();
  Transaction transaction(database_);
  if (auto existing = InspectLifecycleRequestLocked(owner, intent, digest)) {
    if (original_request_digest && existing->status &&
        (!existing->status->selected_bring_up ||
         !existing->status->accepted_intent.replay_equal(intent) ||
         existing->status->selected_bring_up->original_request_payload_digest !=
             *original_request_digest)) {
      transaction.Commit();
      return {RequestReplayStatusDTO::kRequestIdConflict, std::nullopt, std::nullopt};
    }
    transaction.Commit();
    return *existing;
  }
  RecordRejectionLocked(intent.request_context, "component_lifecycle", digest, error, unacquired,
                        intent.program_node_evidence);
  transaction.Commit();
  return {RequestReplayStatusDTO::kAcceptedNew, std::nullopt, std::nullopt, error};
}

CCLCRouteSelectionRequestResultDTO SqliteCCLCJournal::Impl::RecordRouteSelectionRejection(
    const RequestOwnerEpoch& owner, const RouteSelectionIntentDTO& intent, const ErrorDTO& error,
    const std::optional<CellAdmissionClaimIntentDTO>& unacquired) {
  Require(intent.locally_valid() && owner == intent.request_context.request_owner_epoch,
          "component_lifecycle_journal_record_invalid", "no-admission result has invalid identity");
  const auto digest = detail::RouteSelectionSemanticIntentDigest(intent);
  std::lock_guard lock(mutex_);
  RequireOpen();
  Transaction transaction(database_);
  if (auto existing = InspectRouteSelectionRequestLocked(owner, intent, digest)) {
    transaction.Commit();
    return *existing;
  }
  RecordRejectionLocked(intent.request_context, "route_selection", digest, error, unacquired,
                        intent.program_node_evidence);
  transaction.Commit();
  return {RequestReplayStatusDTO::kAcceptedNew, std::nullopt, std::nullopt, error};
}

void SqliteCCLCJournal::Impl::RecordRejectionLocked(
    const PublicRequestContextDTO& context, std::string_view kind, std::string_view digest,
    const ErrorDTO& error, const std::optional<CellAdmissionClaimIntentDTO>& unacquired,
    const std::optional<ProgramNodeAdmissionEvidenceDTO>& evidence) {
  const auto& owner = context.request_owner_epoch;
  Statement pending(
      database_,
      "SELECT lifecycle_transaction_id,claim_generation,lifecycle_accepted,recovery_configuration,"
      "request_payload_digest FROM cclc_lifecycle_cell_claims WHERE owner_epoch=? AND "
      "request_id=?");
  pending.Bind(1, owner.value());
  pending.Bind(2, context.request_id.value());
  const bool has_attempt = pending.Step();
  const auto recovery = has_attempt && !pending.IsNull(3)
                            ? std::optional<std::string>{pending.Blob(3)}
                            : std::nullopt;
  const auto original_digest =
      unacquired && unacquired->owner_transaction_id.starts_with("selected-lifecycle.")
          ? OriginalRequestDigestFromRecovery(*unacquired, recovery)
          : std::nullopt;
  Require((!has_attempt && !unacquired) ||
              (has_attempt && unacquired && pending.IsNull(1) && pending.Integer(2) == 0 &&
               pending.Text(0) == unacquired->owner_transaction_id &&
               unacquired->owner_epoch == owner && unacquired->request_id == context.request_id &&
               (!original_digest || pending.Text(4) == unacquired->request_payload_digest.value()) &&
               (original_digest ? original_digest->value()
                                : unacquired->request_payload_digest.value()) == digest &&
               unacquired->program_node_evidence == evidence &&
               recovery == RecoveryConfigurationBytes(*unacquired, original_digest)),
          "component_lifecycle_rejection_unproven", "request has a crossing or acquired claim");
  Require(!pending.Step(), "component_lifecycle_journal_corrupt", "duplicate claim attempt");
  InsertRejection(owner, context.request_id, kind, digest, error);
  if (unacquired) {
    Statement retire(database_,
                     "DELETE FROM cclc_lifecycle_cell_claims WHERE "
                     "lifecycle_transaction_id=? AND owner_epoch=? AND request_id=? AND "
                     "request_payload_digest=? "
                     "AND claim_generation IS NULL AND lifecycle_accepted=0");
    retire.Bind(1, unacquired->owner_transaction_id);
    retire.Bind(2, owner.value());
    retire.Bind(3, context.request_id.value());
    retire.Bind(4, unacquired->request_payload_digest.value());
    Require(!retire.Step() && sqlite3_changes(database_) == 1,
            "component_lifecycle_claim_receipt_conflict",
            "no-admission result has no exact attempt");
  }
  CompactTerminalResults();
}

void SqliteCCLCJournal::Impl::InsertRejection(const RequestOwnerEpoch& owner,
                                              const RequestId& request_id, std::string_view kind,
                                              std::string_view digest, const ErrorDTO& error) {
  Require(kind == "component_lifecycle" || kind == "route_selection",
          "component_lifecycle_journal_record_invalid", "unknown CCLC request family");
  const auto blob = detail::EncodeCCLCAdmissionRejection(error);
  Statement capacity(
      database_,
      "SELECT request_membership_capacity-(SELECT count(*) FROM cclc_request_memberships m "
      "WHERE m.owner_epoch=e.owner_epoch) FROM cclc_owner_epochs e WHERE owner_epoch=?");
  capacity.Bind(1, owner.value());
  Require(capacity.Step() && capacity.Integer(0) > 0 && !capacity.Step(), "request_ledger_full",
          "no-admission result membership capacity is exhausted");
  CompactTerminalResults(1, blob.size());
  Statement insert(database_,
                   "INSERT INTO cclc_request_memberships "
                   "(owner_epoch,request_id,request_kind,semantic_request_digest,retained_result,"
                   "rejection_blob,rejection_sequence) VALUES(?,?,?,?,1,?,?)");
  insert.Bind(1, owner.value());
  insert.Bind(2, request_id.value());
  insert.Bind(3, kind);
  insert.Bind(4, digest);
  insert.BindBlob(5, blob);
  insert.Bind(6, static_cast<std::int64_t>(MintTerminalSequence()));
  Require(!insert.Step(), "component_lifecycle_journal_corrupt", "rejection insert returned a row");
  // Oversized individual results expire, not violate the retained byte budget.
  CompactTerminalResults();
}

void SqliteCCLCJournal::Impl::RetainRecoveredNoAdmission(const RequestOwnerEpoch& owner,
                                                         std::string_view transaction_id,
                                                         const RequestId& request_id,
                                                         const ContentDigest& digest) {
  const std::string_view family =
      transaction_id.starts_with("route-selection.") ? "route_selection" : "component_lifecycle";
  Statement existing(database_,
                     "SELECT request_kind,semantic_request_digest FROM cclc_request_memberships "
                     "WHERE owner_epoch=? AND request_id=?");
  existing.Bind(1, owner.value());
  existing.Bind(2, request_id.value());
  if (existing.Step()) {
    Require(existing.Text(0) == family && existing.Text(1) == digest.value() && !existing.Step(),
            "component_lifecycle_journal_corrupt", "recovered no-admission identity conflicts");
    return;
  }
  InsertRejection(owner, request_id, family, digest.value(),
                  {"cclc_request_recovered_unstarted",
                   "CCLC owner stopped before accepting this command",
                   {{"effect", "not_started"}, {"reason", "owner_replaced_before_acceptance"}}});
}

void SqliteCCLCJournal::Impl::RetainCurrentComponentLifecycleFact(
    const ComponentLifecycleStatusDTO& status) {
  const auto& component_id = status.accepted_intent.controllable_component_id;
  const auto generation = status.acceptance.component_lifecycle_generation.value();
  Statement watermark(database_,
                      "SELECT component_lifecycle_generation FROM "
                      "lifecycle_component_watermarks WHERE controllable_component_id=?");
  watermark.Bind(1, component_id.value());
  Require(watermark.Step(), "component_lifecycle_journal_generation_conflict",
          "lifecycle status has no component generation watermark");
  const auto current_generation =
      Positive(watermark.Integer(0), "component lifecycle generation watermark");
  Require(!watermark.Step() && generation <= current_generation,
          "component_lifecycle_journal_generation_conflict",
          "lifecycle status exceeds the current component generation");
  if (generation < current_generation) {
    return;
  }

  // A settled partial effect leaves no single coherent component state: the
  // retained predecessor state must not survive as current (restart would
  // otherwise reconstruct it as command readiness). The fact stays absent
  // until an explicit later transition or authorized Apply establishes one.
  if (!status.current_state.has_value() ||
      status.terminal_result == ComponentLifecycleTerminalResultDTO::kFailedPartialEffect) {
    Statement erase(database_,
                    "DELETE FROM lifecycle_component_current_facts "
                    "WHERE controllable_component_id=?");
    erase.Bind(1, component_id.value());
    Require(!erase.Step(), "component_lifecycle_journal_corrupt",
            "current component lifecycle fact deletion returned an unexpected row");
    return;
  }

  Statement retain(database_,
                   "INSERT INTO lifecycle_component_current_facts("
                   "controllable_component_id,component_lifecycle_generation,"
                   "component_lifecycle_state) VALUES(?,?,?) "
                   "ON CONFLICT(controllable_component_id) DO UPDATE SET "
                   "component_lifecycle_generation=excluded.component_lifecycle_generation,"
                   "component_lifecycle_state=excluded.component_lifecycle_state");
  retain.Bind(1, component_id.value());
  retain.Bind(2, static_cast<std::int64_t>(generation));
  retain.Bind(3, static_cast<std::int64_t>(*status.current_state));
  Require(!retain.Step(), "component_lifecycle_journal_corrupt",
          "current component lifecycle fact replacement returned an unexpected row");
}

void SqliteCCLCJournal::Impl::RecordCellClaimAttempt(
    const RequestOwnerEpoch& active_owner_epoch, const CellAdmissionClaimIntentDTO& intent,
    const std::optional<ContentDigest>& original_request_digest) {
  Require(ValidCCLCClaimIntent(intent) && intent.owner_epoch == active_owner_epoch,
          "component_lifecycle_claim_invalid", "lifecycle claim intent is invalid");
  const auto recovery = RecoveryConfigurationBytes(intent, original_request_digest);
  std::lock_guard lock(mutex_);
  RequireOpen();
  Transaction transaction(database_);
  Require(ActiveOwner(active_owner_epoch),
          "component_lifecycle_journal_owner_epoch_stale",
          "lifecycle claim writer does not own the active epoch");
  Require(ScalarInteger(database_,
                        "SELECT count(*) FROM cclc_lifecycle_cell_claims") <
              static_cast<std::int64_t>(kMaximumCellAdmissionClaims),
          "component_lifecycle_claim_capacity_exhausted",
          "bounded lifecycle claim journal is full");
  Statement insert(database_,
                   "INSERT INTO cclc_lifecycle_cell_claims("
                   "lifecycle_transaction_id,owner_epoch,request_id,"
                   "request_payload_digest,expected_admission_generation,program_node_evidence,"
                   "recovery_configuration)"
                   " VALUES(?,?,?,?,?,?,?)");
  insert.Bind(1, intent.owner_transaction_id);
  insert.Bind(2, intent.owner_epoch.value());
  insert.Bind(3, intent.request_id.value());
  insert.Bind(4, intent.request_payload_digest.value());
  insert.Bind(5, static_cast<std::int64_t>(
                     intent.expected_admission_generation.value()));
  if (intent.program_node_evidence) {
    insert.BindBlob(6, detail::EncodeProgramNodeAdmissionEvidence(*intent.program_node_evidence));
  } else {
    insert.BindNull(6);
  }
  if (recovery)
    insert.BindBlob(7, *recovery);
  else
    insert.BindNull(7);
  Require(!insert.Step(), "component_lifecycle_claim_conflict",
          "lifecycle claim attempt was already recorded");
  transaction.Commit();
}

std::optional<std::string> SqliteCCLCJournal::Impl::ReadClaimRecoveryLocked(
    const CellAdmissionClaimIntentDTO& intent) {
  Statement row(database_,
                "SELECT recovery_configuration FROM cclc_lifecycle_cell_claims "
                "WHERE lifecycle_transaction_id=?");
  row.Bind(1, intent.owner_transaction_id);
  Require(row.Step(), "component_lifecycle_claim_missing", "claim attempt is missing");
  auto bytes = row.IsNull(0) ? std::optional<std::string>{} : row.Blob(0);
  Require(!row.Step(), "component_lifecycle_journal_corrupt", "duplicate claim attempt");
  (void)OriginalRequestDigestFromRecovery(intent, bytes);
  return bytes;
}

ContentDigest SqliteCCLCJournal::Impl::ReadOriginalRequestDigestLocked(
    std::string_view transaction_id, const ContentDigest& offer_digest) {
  if (!transaction_id.starts_with("selected-lifecycle.")) return offer_digest;
  Statement row(database_,
                "SELECT owner_epoch,request_id,expected_admission_generation,program_node_evidence,"
                "recovery_configuration FROM cclc_lifecycle_cell_claims "
                "WHERE lifecycle_transaction_id=? AND request_payload_digest=?");
  row.Bind(1, transaction_id);
  row.Bind(2, offer_digest.value());
  Require(row.Step() && !row.IsNull(4), "component_lifecycle_journal_corrupt",
          "selected original request identity is missing");
  const auto record = detail::DecodeCCLCSelectedBringUpClaimIntent(row.Blob(4));
  const auto node = row.IsNull(3) ? std::optional<ProgramNodeAdmissionEvidenceDTO>{}
                                  : detail::DecodeProgramNodeAdmissionEvidence(row.Blob(3));
  const auto& intent = record.full_claim_intent;
  Require(intent.owner_transaction_id == transaction_id &&
              intent.request_payload_digest == offer_digest &&
              intent.owner_epoch.value() == row.Text(0) &&
              intent.request_id.value() == row.Text(1) &&
              intent.expected_admission_generation.value() ==
                  Positive(row.Integer(2), "selected claim admission generation") &&
              intent.program_node_evidence == node && !row.Step(),
          "component_lifecycle_journal_corrupt",
          "selected original request identity crosses its full SQL claim intent");
  return record.original_request_payload_digest;
}

void SqliteCCLCJournal::Impl::RecordCellClaimAcquired(const RequestOwnerEpoch& active_owner_epoch,
                                                      const CellAdmissionClaimDTO& claim) {
  Require(ValidCCLCClaimIntent(claim.intent), "component_lifecycle_claim_invalid",
          "lifecycle claim is invalid");
  std::lock_guard lock(mutex_);
  RequireOpen();
  Transaction transaction(database_);
  Require(ActiveOwner(active_owner_epoch),
          "component_lifecycle_journal_owner_epoch_stale",
          "lifecycle claim writer does not own the active epoch");
  Statement update(database_,
                   "UPDATE cclc_lifecycle_cell_claims SET claim_generation=?,"
                   "acquired_admission_generation=?,accepted_at_ns=? "
                   "WHERE lifecycle_transaction_id=? AND owner_epoch=? AND request_id=? "
                   "AND request_payload_digest=? AND expected_admission_generation=? "
                   "AND claim_generation IS NULL AND program_node_evidence IS ? AND "
                   "recovery_configuration IS ?");
  update.Bind(1, static_cast<std::int64_t>(claim.claim_generation.value()));
  update.Bind(2, static_cast<std::int64_t>(
                     claim.acquired_admission_generation.value()));
  update.Bind(3, claim.accepted_at_ns);
  update.Bind(4, claim.intent.owner_transaction_id);
  update.Bind(5, claim.intent.owner_epoch.value());
  update.Bind(6, claim.intent.request_id.value());
  update.Bind(7, claim.intent.request_payload_digest.value());
  update.Bind(8, static_cast<std::int64_t>(
                     claim.intent.expected_admission_generation.value()));
  if (claim.intent.program_node_evidence) {
    update.BindBlob(
        9, detail::EncodeProgramNodeAdmissionEvidence(*claim.intent.program_node_evidence));
  } else {
    update.BindNull(9);
  }
  const auto recovery = claim.intent.owner_transaction_id.starts_with("selected-lifecycle.")
                            ? ReadClaimRecoveryLocked(claim.intent)
                            : RecoveryConfigurationBytes(claim.intent);
  if (recovery)
    update.BindBlob(10, *recovery);
  else
    update.BindNull(10);
  Require(!update.Step() && sqlite3_changes(database_) == 1,
          "component_lifecycle_claim_conflict",
          "exact lifecycle claim does not match its durable attempt");
  transaction.Commit();
}

void SqliteCCLCJournal::Impl::BindCellClaim(
    const PublicRequestContextDTO& request, std::string_view transaction_id,
    AdmissionGeneration admission, std::string_view semantic_digest,
    const std::optional<CellAdmissionConfigurationScopeDTO>& recovery) {
  Require(admission.nonzero(), "component_lifecycle_claim_missing",
          "new CCLC acceptance requires admission");
  Statement bind(database_,
                 "UPDATE cclc_lifecycle_cell_claims SET lifecycle_accepted=1 "
                 "WHERE lifecycle_transaction_id=? AND owner_epoch=? AND request_id=? "
                 "AND expected_admission_generation=? AND claim_generation IS NOT NULL "
                 "AND lifecycle_accepted=0 AND request_payload_digest=? "
                 "AND (recovery_configuration IS NULL OR recovery_configuration=?)");
  bind.Bind(1, transaction_id);
  bind.Bind(2, request.request_owner_epoch.value());
  bind.Bind(3, request.request_id.value());
  bind.Bind(4, static_cast<std::int64_t>(admission.value()));
  bind.Bind(5, semantic_digest);
  if (recovery)
    bind.BindBlob(6, detail::EncodeCCLCRouteRecoveryConfiguration(*recovery));
  else
    bind.BindNull(6);
  Require(!bind.Step() && sqlite3_changes(database_) == 1, "component_lifecycle_claim_missing",
          "CCLC acceptance has no exact durable Kernel claim");
}

void SqliteCCLCJournal::Impl::BindSelectedCellClaim(const ComponentLifecycleStatusDTO& status) {
  const auto& evidence = *status.selected_bring_up;
  const auto& claim = evidence.selected_claim;
  const auto incident =
      ReadOpenComponentIncidentLocked(status.accepted_intent.controllable_component_id);
  Require(
      incident && incident->incident.incident_id == evidence.incident_id &&
          incident->incident.containment_state ==
              ComponentIncidentContainmentStateDTO::kRequested &&
          incident->incident.containment &&
          incident->incident.containment->stop_id == evidence.containment_stop_id &&
          incident->incident.containment->stop_generation == evidence.containment_stop_generation,
      "selected_bring_up_incident_changed",
      "selected bring-up no longer names the current contained open incident");
  Require(FitsSqliteInteger(claim.claim_generation.value()) &&
              FitsSqliteInteger(claim.acquired_admission_generation.value()),
          "component_lifecycle_journal_record_invalid",
          "selected claim counters exceed the journal representation");
  Statement bind(database_,
                 "UPDATE cclc_lifecycle_cell_claims SET lifecycle_accepted=1 "
                 "WHERE lifecycle_transaction_id=? AND owner_epoch=? AND request_id=? "
                 "AND expected_admission_generation=? AND request_payload_digest=? "
                 "AND claim_generation=? AND acquired_admission_generation=? AND accepted_at_ns=? "
                 "AND lifecycle_accepted=0 AND release_kind IS NULL AND release_receipt_recorded=0 "
                 "AND program_node_evidence IS NULL AND recovery_configuration=?");
  bind.Bind(1, claim.intent.owner_transaction_id);
  bind.Bind(2, claim.intent.owner_epoch.value());
  bind.Bind(3, claim.intent.request_id.value());
  bind.Bind(4, static_cast<std::int64_t>(claim.intent.expected_admission_generation.value()));
  bind.Bind(5, claim.intent.request_payload_digest.value());
  bind.Bind(6, static_cast<std::int64_t>(claim.claim_generation.value()));
  bind.Bind(7, static_cast<std::int64_t>(claim.acquired_admission_generation.value()));
  bind.Bind(8, claim.accepted_at_ns);
  bind.BindBlob(9, detail::EncodeCCLCSelectedBringUpClaimIntent(
                       {claim.intent, evidence.original_request_payload_digest}));
  Require(!bind.Step() && sqlite3_changes(database_) == 1, "component_lifecycle_claim_missing",
          "selected acceptance has no exact unreleased durable Kernel claim");
}

void SqliteCCLCJournal::Impl::RequireApplyBringUpAcceptanceLocked(
    const ComponentLifecycleStatusDTO& status) {
  const auto& evidence = *status.apply_bring_up;
  const auto& parent = evidence.parent_authority;
  Require(parent.valid() &&
              status.accepted_intent.expected_admission_generation ==
                  parent.parent_claim.intent.expected_admission_generation,
          "apply_bring_up_parent_identity_invalid",
          "delegated component expectation does not match its parent admission");
  Statement independent(database_,
      "SELECT 1 FROM cclc_lifecycle_cell_claims WHERE lifecycle_transaction_id=? OR "
      "(owner_epoch=? AND request_id=?)");
  independent.Bind(1, status.acceptance.lifecycle_transaction_id.value());
  independent.Bind(2, status.accepted_intent.request_context.request_owner_epoch.value());
  independent.Bind(3, status.accepted_intent.request_context.request_id.value());
  Require(!independent.Step(), "apply_bring_up_claim_conflict",
          "delegated component cannot also own an independent cell claim");
  const auto incident =
      ReadOpenComponentIncidentLocked(status.accepted_intent.controllable_component_id);
  if (evidence.recovery) {
    const auto& recovery = *evidence.recovery;
    Require(incident && incident->incident.incident_id == recovery.incident_id &&
                incident->incident.containment_state == ComponentIncidentContainmentStateDTO::kRequested &&
                incident->incident.containment &&
                incident->incident.containment->stop_id == recovery.containment_stop_id &&
                incident->incident.containment->stop_generation == recovery.containment_stop_generation,
            "apply_bring_up_incident_changed",
            "delegated recovery no longer names the current contained open incident");
  } else {
    Require(!incident, "apply_bring_up_incident_changed",
            "clean delegated startup cannot ignore a current open incident");
  }
}

CCLCLifecycleRequestResultDTO SqliteCCLCJournal::Impl::AcceptOrReplayLifecycle(
    const RequestOwnerEpoch& active_owner_epoch, const ComponentLifecycleIntentDTO& intent,
    const ComponentLifecycleAcceptedDTO& candidate_acceptance,
    const ComponentLifecycleStatusDTO& candidate_initial_status,
    const ContentDigest* original_request_digest, bool apply_delegated,
    const ComponentLifecycleTargetExpectationDTO* expected_target,
    const CellAdmissionClaimDTO* acquired_claim) {
  const bool selected = original_request_digest != nullptr && !apply_delegated;
  Require((!apply_delegated || original_request_digest != nullptr) &&
              apply_delegated == candidate_initial_status.apply_bring_up.has_value() &&
              selected == candidate_initial_status.selected_bring_up.has_value(),
          "component_lifecycle_journal_record_invalid",
          "selected evidence requires the private ledger entry");
  Require(!active_owner_epoch.empty() && intent.locally_valid() &&
              !intent.close_incident_recovery &&
              (apply_delegated || !intent.apply_generation_selection),
          "component_lifecycle_journal_record_invalid",
          "lifecycle request identity or semantic input is invalid");
  const auto semantic_digest = original_request_digest ? original_request_digest->value()
                                        : detail::ComponentLifecycleSemanticIntentDigest(intent);
  Require(!selected ||
              (IsExactLowercaseSha256Digest(semantic_digest) &&
               *original_request_digest ==
                   candidate_initial_status.selected_bring_up->original_request_payload_digest),
          "component_lifecycle_journal_record_invalid", "selected offer digest is invalid");

  Require(!apply_delegated ||
              (IsExactLowercaseSha256Digest(semantic_digest) && *original_request_digest ==
                  candidate_initial_status.apply_bring_up->original_request_payload_digest),
          "component_lifecycle_journal_record_invalid", "delegated request digest is invalid");
  RefreshNativeApplyPreparationFailure();
  std::lock_guard lock(mutex_);
  RequireOpen();
  Transaction transaction(database_);
  auto existing = InspectLifecycleRequestLocked(active_owner_epoch, intent, semantic_digest);
  if (existing.has_value()) {
    if (existing->status &&
        (existing->status->selected_bring_up.has_value() != selected ||
         existing->status->apply_bring_up.has_value() != apply_delegated ||
         (original_request_digest && !existing->status->accepted_intent.replay_equal(intent)))) {
      transaction.Commit();
      return {RequestReplayStatusDTO::kRequestIdConflict, std::nullopt, std::nullopt};
    }
    transaction.Commit();
    return std::move(*existing);
  }
  if (expected_target) {
    Require(!selected && !apply_delegated && acquired_claim && expected_target->locally_valid(),
            "component_lifecycle_journal_record_invalid", "target expectation is invalid");
    Require(candidate_acceptance.locally_valid() && candidate_initial_status.locally_valid() &&
                candidate_acceptance.replay_status == RequestReplayStatusDTO::kAcceptedNew &&
                candidate_initial_status.acceptance == candidate_acceptance &&
                candidate_initial_status.accepted_intent == intent &&
                candidate_initial_status.status_owner_epoch == active_owner_epoch &&
                candidate_initial_status.status_sequence == 1U &&
                candidate_initial_status.phase == ComponentLifecycleTransactionPhaseDTO::kAccepted &&
                !candidate_initial_status.terminal_result,
            "component_lifecycle_journal_record_invalid", "target admission candidate is invalid");
    RequireTargetClaimLocked(active_owner_epoch, intent, candidate_acceptance, *acquired_claim);
    const auto target = ReadLifecycleTargetLocked(intent.controllable_component_id);
    const bool component_matches =
        expected_target->controllable_component_id == intent.controllable_component_id;
    const bool present = target && target->second;
    if (!component_matches || !present ||
        target->first.target_generation != expected_target->target_generation) {
      const ErrorDTO refusal{
          "component_lifecycle_target_stale", "the original lifecycle target no longer matches",
          {{"effect", "not_started"},
           {"reason", !component_matches ? "target_component_mismatch" :
                      !present ? "target_retired" : "target_generation_changed"}}};
      // One commit consumes the original request and retains actual no-effect
      // settlement. An acquired claim must never use RecordRejectionLocked's
      // unacquired-attempt disposition.
      InsertRejection(active_owner_epoch, intent.request_context.request_id,
                      "component_lifecycle", semantic_digest, refusal);
      const auto& claim = *acquired_claim;
      const CellAdmissionClaimReleaseDTO release{
          claim.intent.owner_role, claim.intent.owner_epoch, claim.intent.owner_transaction_id,
          claim.intent.request_id, claim.intent.request_payload_digest,
          claim.acquired_admission_generation, claim.claim_generation,
          {CellAdmissionSettlementKindDTO::kProvenNoEffect, claim.intent.request_payload_digest}};
      RecordUnacceptedCellClaimReleaseLocked(active_owner_epoch, release);
      transaction.Commit();
      return {RequestReplayStatusDTO::kAcceptedNew, std::nullopt, std::nullopt, refusal};
    }
  }
  const auto pending_apply = ReadUnsettledNativeApplyPlanLocked();
  Require(!apply_delegated || intent.apply_generation_selection.has_value(),
          "component_lifecycle_journal_record_invalid",
          "fresh delegated acceptance requires its exact owner generation selection");
  if (pending_apply &&
      (!apply_delegated ||
       pending_apply->apply_id != candidate_initial_status.apply_bring_up->parent_authority.parent.owner.apply_id ||
       pending_apply->apply_generation != candidate_initial_status.apply_bring_up->parent_authority.parent.owner.apply_generation)) {
    Fail("component_lifecycle_native_apply_unsettled",
         "native Apply " + pending_apply->apply_id.value() + "/" +
             std::to_string(pending_apply->apply_generation.value()) +
             " blocks new lifecycle acceptance");
  }

  Require(candidate_acceptance.locally_valid() && candidate_initial_status.locally_valid() &&
              candidate_acceptance.replay_status == RequestReplayStatusDTO::kAcceptedNew &&
              candidate_initial_status.acceptance == candidate_acceptance &&
              candidate_initial_status.accepted_intent == intent &&
              candidate_initial_status.status_owner_epoch == active_owner_epoch &&
              candidate_initial_status.status_sequence == 1U &&
              candidate_initial_status.phase == ComponentLifecycleTransactionPhaseDTO::kAccepted &&
              !candidate_initial_status.terminal_result.has_value() &&
              FitsSqliteInteger(candidate_acceptance.lifecycle_transaction_generation.value()) &&
              FitsSqliteInteger(intent.expected_component_lifecycle_generation.value()) &&
              FitsSqliteInteger(candidate_acceptance.component_lifecycle_generation.value()),
          "component_lifecycle_journal_record_invalid",
          "unseen lifecycle acceptance candidate is invalid");
  const auto status_blob = detail::EncodeComponentLifecycleStatus(candidate_initial_status);
  if (apply_delegated)
    RequireApplyBringUpAcceptanceLocked(candidate_initial_status);
  else if (selected)
    BindSelectedCellClaim(candidate_initial_status);
  else
    BindCellClaim(intent.request_context, candidate_acceptance.lifecycle_transaction_id.value(),
                  intent.expected_admission_generation, semantic_digest);

  const auto active_count = ActiveTransactionCount();
  if (active_count >= kMaximumCCLCActiveTransactions) {
    Fail("request_queue_full", "CCLC active nonterminal transaction capacity is exhausted");
  }

  Statement metadata(database_,
                     "SELECT highest_transaction_generation FROM cclc_journal_metadata "
                     "WHERE singleton=1");
  Require(metadata.Step(), "component_lifecycle_journal_corrupt",
          "lifecycle transaction watermark is missing");
  const auto highest = Nonnegative(metadata.Integer(0), "highest lifecycle transaction generation");
  Require(!metadata.Step() &&
              highest < static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) &&
              candidate_acceptance.lifecycle_transaction_generation.value() == highest + 1U,
          "component_lifecycle_journal_generation_conflict",
          "lifecycle transaction generation is not the next durable value");

  Statement component(database_,
                      "SELECT component_lifecycle_generation FROM "
                      "lifecycle_component_watermarks WHERE controllable_component_id=?");
  component.Bind(1, intent.controllable_component_id.value());
  const bool component_seen = component.Step();
  const auto expected_component_generation = intent.expected_component_lifecycle_generation.value();
  const auto candidate_component_generation =
      candidate_acceptance.component_lifecycle_generation.value();
  const bool disable = intent.intent == ComponentLifecycleIntentKindDTO::kDisable;
  std::uint64_t component_predecessor = 0U;
  if (component_seen) {
    component_predecessor =
        Positive(component.Integer(0), "component lifecycle generation watermark");
    Require(!component.Step(), "component_lifecycle_journal_corrupt",
            "component lifecycle watermark is duplicated");
    Require(disable && !apply_delegated ? expected_component_generation <= component_predecessor
                                       : expected_component_generation == component_predecessor,
            "component_lifecycle_journal_generation_conflict",
            "component lifecycle expectation does not match its durable watermark");
  } else {
    Statement component_count(database_, "SELECT count(*) FROM lifecycle_component_watermarks");
    Require(component_count.Step(), "component_lifecycle_journal_corrupt",
            "component lifecycle watermark count is missing");
    const auto count =
        Nonnegative(component_count.Integer(0), "component lifecycle watermark count");
    Require(!component_count.Step(), "component_lifecycle_journal_corrupt",
            "component lifecycle watermark count is duplicated");
    if (count >= kMaximumCCLCComponentWatermarks) {
      Fail("component_lifecycle_journal_capacity_exhausted",
           "component lifecycle watermark capacity is exhausted");
    }

    // Resolved admission has already selected an exact current snapshot and
    // carries its successor in the candidate. An unresolved Full-Stop Disable
    // has no such snapshot, so its immutable caller expectation is the only
    // legal one-time seed for containment-only numbering.
    if (apply_delegated) {
      component_predecessor = intent.apply_generation_selection->owner_qualified_predecessor
          ? intent.apply_generation_selection->owner_qualified_predecessor->generation.value() : 0U;
    } else if (disable && candidate_initial_status.current_state.has_value()) {
      component_predecessor = candidate_component_generation - 1U;
    } else {
      component_predecessor = expected_component_generation;
    }
  }
  if (apply_delegated) {
    const auto selected_generation = SelectApplyComponentGenerationLocked(
        intent.controllable_component_id.value(), intent.apply_generation_selection->owner_qualified_predecessor);
    Require(selected_generation == *intent.apply_generation_selection &&
                expected_component_generation == component_predecessor &&
                candidate_component_generation == selected_generation.candidate_generation.value(),
            "component_lifecycle_journal_generation_conflict",
            "delegated generation selection changed before acceptance");
  } else {
    Require(component_predecessor <
                  static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) &&
              candidate_component_generation == component_predecessor + 1U,
          "component_lifecycle_journal_generation_conflict",
          "component lifecycle generation is not the immediate durable successor");
  }

  Require(candidate_acceptance.lifecycle_transaction_id.value() ==
              (apply_delegated ? "apply-lifecycle." : selected ? "selected-lifecycle." : "lifecycle.") +
                  std::to_string(candidate_acceptance.lifecycle_transaction_generation.value()),
          "component_lifecycle_journal_transaction_identity_reused",
          "lifecycle identity must name its exact immediate-successor generation");
  Statement guarded_identity(database_,
                             "SELECT lifecycle_transaction_generation FROM "
                             "cclc_lifecycle_transaction_identity_guards WHERE "
                             "lifecycle_transaction_id=?");
  guarded_identity.Bind(1, candidate_acceptance.lifecycle_transaction_id.value());
  Require(!guarded_identity.Step(), "component_lifecycle_journal_transaction_identity_reused",
          "lifecycle transaction identity was already used");

  Statement insert_membership(database_,
                              "INSERT INTO cclc_request_memberships("
                              "owner_epoch,request_id,request_kind,semantic_request_digest,"
                              "retained_result) VALUES(?,?,'component_lifecycle',?,1)");
  insert_membership.Bind(1, active_owner_epoch.value());
  insert_membership.Bind(2, intent.request_context.request_id.value());
  insert_membership.Bind(3, semantic_digest);
  Require(!insert_membership.Step(), "component_lifecycle_journal_corrupt",
          "CCLC request membership insert returned an unexpected row");
  const auto membership_sequence = sqlite3_last_insert_rowid(database_);
  Require(membership_sequence > 0, "component_lifecycle_journal_corrupt",
          "CCLC request membership sequence is invalid");

  Statement insert_identity_guard(database_,
                                  "INSERT INTO cclc_lifecycle_transaction_identity_guards("
                                  "lifecycle_transaction_id,lifecycle_transaction_generation) "
                                  "VALUES(?,?)");
  insert_identity_guard.Bind(1, candidate_acceptance.lifecycle_transaction_id.value());
  insert_identity_guard.Bind(
      2, static_cast<std::int64_t>(candidate_acceptance.lifecycle_transaction_generation.value()));
  Require(!insert_identity_guard.Step(), "component_lifecycle_journal_transaction_identity_reused",
          "lifecycle transaction identity or generation was already used");

  Statement insert(database_,
                   "INSERT INTO lifecycle_transactions("
                   "membership_sequence,lifecycle_transaction_id,"
                   "lifecycle_transaction_generation,controllable_component_id,"
                   "component_lifecycle_generation,status_sequence,accepted_at_ns,"
                   "latest_updated_at_ns,terminal,terminal_sequence,status_blob) "
                   "VALUES(?,?,?,?,?,?,?,?,?,NULL,?)");
  insert.Bind(1, membership_sequence);
  insert.Bind(2, candidate_acceptance.lifecycle_transaction_id.value());
  insert.Bind(
      3, static_cast<std::int64_t>(candidate_acceptance.lifecycle_transaction_generation.value()));
  insert.Bind(4, intent.controllable_component_id.value());
  insert.Bind(
      5, static_cast<std::int64_t>(candidate_acceptance.component_lifecycle_generation.value()));
  insert.Bind(6, static_cast<std::int64_t>(candidate_initial_status.status_sequence));
  insert.Bind(7, candidate_initial_status.accepted_at_ns);
  insert.Bind(8, candidate_initial_status.latest_updated_at_ns);
  insert.Bind(9, Terminal(candidate_initial_status) ? 1 : 0);
  insert.BindBlob(10, status_blob);
  Require(!insert.Step(), "component_lifecycle_journal_corrupt",
          "lifecycle transaction insert returned an unexpected row");

  Statement update_metadata(database_,
                            "UPDATE cclc_journal_metadata SET highest_transaction_generation=? "
                            "WHERE singleton=1");
  update_metadata.Bind(
      1, static_cast<std::int64_t>(candidate_acceptance.lifecycle_transaction_generation.value()));
  Require(!update_metadata.Step(), "component_lifecycle_journal_corrupt",
          "lifecycle transaction watermark update returned an unexpected row");

  Statement update_component(
      database_,
      "INSERT INTO lifecycle_component_watermarks("
      "controllable_component_id,component_lifecycle_generation) VALUES(?,?) "
      "ON CONFLICT(controllable_component_id) DO UPDATE SET "
      "component_lifecycle_generation=excluded.component_lifecycle_generation");
  update_component.Bind(1, intent.controllable_component_id.value());
  update_component.Bind(
      2, static_cast<std::int64_t>(candidate_acceptance.component_lifecycle_generation.value()));
  Require(!update_component.Step(), "component_lifecycle_journal_corrupt",
          "component lifecycle watermark update returned an unexpected row");

  RetainCurrentComponentLifecycleFact(candidate_initial_status);
  if (!selected && !apply_delegated && !expected_target) RecordAcceptedOperatorRequestLocked(intent);

  CompactTerminalResults();
  transaction.Commit();
  return CCLCLifecycleRequestResultDTO{RequestReplayStatusDTO::kAcceptedNew, candidate_acceptance,
                                       candidate_initial_status};
}

void SqliteCCLCJournal::Impl::RequireTargetClaimLocked(
    const RequestOwnerEpoch& owner, const ComponentLifecycleIntentDTO& intent,
    const ComponentLifecycleAcceptedDTO& acceptance, const CellAdmissionClaimDTO& claim) {
  const CellAdmissionClaimIntentDTO expected{
      CellAdmissionClaimKindDTO::kComponentLifecycle,
      CellAdmissionOwnerRoleDTO::kComponentLifecycleOwner, owner,
      acceptance.lifecycle_transaction_id.value(), intent.request_context.request_id,
      ContentDigest(detail::ComponentLifecycleSemanticIntentDigest(intent)),
      intent.expected_admission_generation, {}, std::nullopt, std::nullopt,
      intent.program_node_evidence};
  Require(ValidCCLCClaimIntent(expected) && claim.intent == expected &&
              ValidateCellAdmissionRecoveryReceiptQuery({expected, claim, std::nullopt}).ok(),
          "component_lifecycle_claim_conflict", "target admission has a foreign acquired claim");
  Statement row(database_,
      "SELECT owner_epoch,request_id,request_payload_digest,expected_admission_generation,"
      "claim_generation,acquired_admission_generation,accepted_at_ns,program_node_evidence,"
      "recovery_configuration,lifecycle_accepted,release_kind,release_receipt_recorded,"
      "(SELECT count(*) FROM lifecycle_transactions t WHERE t.lifecycle_transaction_id=c.lifecycle_transaction_id)+"
      "(SELECT count(*) FROM route_selection_transactions t WHERE t.route_selection_transaction_id=c.lifecycle_transaction_id)+"
      "(SELECT count(*) FROM cclc_effect_attempts e WHERE e.lifecycle_transaction_id=c.lifecycle_transaction_id)+"
      "(SELECT count(*) FROM cclc_route_effect_attempts e WHERE e.route_selection_transaction_id=c.lifecycle_transaction_id) "
      "FROM cclc_lifecycle_cell_claims c WHERE lifecycle_transaction_id=?");
  row.Bind(1, expected.owner_transaction_id);
  Require(row.Step(), "component_lifecycle_claim_missing", "target admission claim is missing");
  const auto node = row.IsNull(7) ? std::optional<ProgramNodeAdmissionEvidenceDTO>{}
                                : detail::DecodeProgramNodeAdmissionEvidence(row.Blob(7));
  const auto recovery = row.IsNull(8) ? std::optional<std::string>{} : row.Blob(8);
  Require(row.Text(0) == owner.value() && row.Text(1) == expected.request_id.value() &&
              row.Text(2) == expected.request_payload_digest.value() &&
              row.Integer(3) == static_cast<std::int64_t>(expected.expected_admission_generation.value()) &&
              !row.IsNull(4) && row.Integer(4) == static_cast<std::int64_t>(claim.claim_generation.value()) &&
              !row.IsNull(5) && row.Integer(5) == static_cast<std::int64_t>(claim.acquired_admission_generation.value()) &&
              !row.IsNull(6) && row.Integer(6) == claim.accepted_at_ns &&
              node == expected.program_node_evidence && recovery == RecoveryConfigurationBytes(expected) &&
              row.Integer(9) == 0 && row.IsNull(10) && row.Integer(11) == 0 && row.Integer(12) == 0 &&
              !row.Step(), "component_lifecycle_claim_conflict",
          "target admission claim is not exact and unaccepted with zero effects");
}

void SqliteCCLCJournal::Impl::RecordAcceptedOperatorRequestLocked(
    const ComponentLifecycleIntentDTO& intent) {
  if (intent.request_context.source_label == kLifecycleTargetSourceLabel) return;
  if (intent.intent == ComponentLifecycleIntentKindDTO::kRecover) {
    // ADR-0026 / spec 220: only the operator's accepted Recover clears the
    // component's retained intervention-requiring causes, in its acceptance
    // commit. With an open incident that is RecoverComponentIncident; this
    // is the ordinary Recover without one. Exact replay never reaches here.
    Statement causes(database_,
                     "DELETE FROM cclc_component_intervention_causes "
                     "WHERE controllable_component_id=?");
    causes.Bind(1, intent.controllable_component_id.value());
    Require(!causes.Step(), "component_lifecycle_journal_corrupt",
            "intervention cause removal returned a row");
    // A driver pass that read the causes before this commit may not retain
    // one again, nor open an incident from them.
    AdvanceRecoveryGenerationLocked(intent.controllable_component_id);
    return;
  }
  // ADR-0026 §7 / spec 210: an admitted operator Prepare, Enable or Disable
  // records its target in the same commit as its acceptance, so neither can
  // exist without the other. System-attributed (target-following) requests
  // name no target. M configuration creates no target: a first explicit
  // command must positively join the exact adopted component instead.
  const auto requested = lifecycle_target_of(intent.intent);
  if (!requested) return;
  const auto existing = ReadLifecycleTargetLocked(intent.controllable_component_id);
  if (!existing || !existing->second) {
    Statement adopted(database_,
        "SELECT 1 FROM cclc_native_apply_plans p "
        "JOIN cclc_native_apply_adoptions a ON a.apply_id=p.apply_id AND a.apply_generation=p.apply_generation "
        "AND a.candidate_bundle_id=p.candidate_bundle_id AND a.candidate_graph_revision=p.candidate_graph_revision "
        "JOIN cclc_native_apply_plan_components c ON c.controllable_component_id=? "
        "JOIN lifecycle_component_current_facts f ON f.controllable_component_id=c.controllable_component_id "
        "JOIN lifecycle_component_watermarks w ON w.controllable_component_id=f.controllable_component_id "
        "AND w.component_lifecycle_generation=f.component_lifecycle_generation "
        "WHERE p.singleton=1 AND p.component_bring_up IS NULL "
        "AND f.component_lifecycle_generation>=c.component_lifecycle_generation");
    adopted.Bind(1, intent.controllable_component_id.value());
    if (!adopted.Step()) return;
    Require(!adopted.Step(), "component_lifecycle_journal_corrupt",
            "operator target adopted component membership is duplicated");
  }
  static_cast<void>(WriteLifecycleTargetLocked(intent.controllable_component_id, *requested,
                                               LifecycleTargetOriginDTO::kOperator, false));
}

std::optional<CCLCRouteSelectionRequestResultDTO>
SqliteCCLCJournal::Impl::InspectRouteSelectionRequestLocked(
    const RequestOwnerEpoch& active_owner_epoch, const RouteSelectionIntentDTO& intent,
    std::string_view semantic_digest) {
  if (intent.request_context.request_owner_epoch != active_owner_epoch ||
      !ActiveOwner(active_owner_epoch)) {
    return CCLCRouteSelectionRequestResultDTO{RequestReplayStatusDTO::kStaleRequestOwnerEpoch,
                                              std::nullopt, std::nullopt};
  }

  auto result = FindRouteSelectionRequestResultLocked(intent, semantic_digest);
  if (!result && MembershipFull(active_owner_epoch)) {
    return CCLCRouteSelectionRequestResultDTO{RequestReplayStatusDTO::kRequestLedgerFull,
                                              std::nullopt, std::nullopt};
  }
  return result;
}

std::optional<CCLCRouteSelectionRequestResultDTO>
SqliteCCLCJournal::Impl::FindRouteSelectionRequestResultLocked(
    const RouteSelectionIntentDTO& intent, std::string_view semantic_digest) {
  Statement membership_lookup(database_,
                              "SELECT membership_sequence,request_kind,semantic_request_digest,"
                              "retained_result,rejection_blob FROM cclc_request_memberships "
                              "WHERE owner_epoch=? AND request_id=?");
  membership_lookup.Bind(1, intent.request_context.request_owner_epoch.value());
  membership_lookup.Bind(2, intent.request_context.request_id.value());
  if (!membership_lookup.Step()) {
    return std::nullopt;
  }
  const auto membership_sequence =
      Positive(membership_lookup.Integer(0), "request membership sequence");
  const auto request_kind = membership_lookup.Text(1);
  const auto retained_digest = membership_lookup.Text(2);
  const auto retained_result = membership_lookup.Integer(3);
  const auto rejection = membership_lookup.IsNull(4)
                             ? std::optional<ErrorDTO>{}
                             : detail::DecodeCCLCAdmissionRejection(membership_lookup.Blob(4));
  Require((retained_result == 0 || retained_result == 1) && !membership_lookup.Step(),
          "component_lifecycle_journal_corrupt",
          "CCLC route request membership is invalid or duplicated");
  if (request_kind != "route_selection" || retained_digest != semantic_digest) {
    return CCLCRouteSelectionRequestResultDTO{RequestReplayStatusDTO::kRequestIdConflict,
                                              std::nullopt, std::nullopt};
  }
  if (retained_result == 0) {
    return CCLCRouteSelectionRequestResultDTO{RequestReplayStatusDTO::kRequestResultExpired,
                                              std::nullopt, std::nullopt};
  }
  if (rejection) {
    return CCLCRouteSelectionRequestResultDTO{RequestReplayStatusDTO::kReplayedExact, std::nullopt,
                                              std::nullopt, rejection};
  }
  Statement retained(database_,
                     "SELECT status_blob FROM route_selection_transactions "
                     "WHERE membership_sequence=?");
  retained.Bind(1, static_cast<std::int64_t>(membership_sequence));
  Require(retained.Step(), "component_lifecycle_journal_corrupt",
          "retained route request has no transaction result");
  auto status = detail::DecodeRouteSelectionStatus(retained.Blob(0));
  Require(!retained.Step(), "component_lifecycle_journal_corrupt",
          "retained route request has duplicate transaction results");
  auto acceptance = status.acceptance;
  acceptance.replay_status = RequestReplayStatusDTO::kReplayedExact;
  return CCLCRouteSelectionRequestResultDTO{RequestReplayStatusDTO::kReplayedExact,
                                            std::move(acceptance), std::move(status)};
}

std::optional<CCLCRouteSelectionRequestResultDTO>
SqliteCCLCJournal::Impl::InspectRouteSelectionRequest(const RequestOwnerEpoch& active_owner_epoch,
                                                      const RouteSelectionIntentDTO& intent) {
  Require(!active_owner_epoch.empty() && intent.locally_valid(),
          "component_lifecycle_journal_record_invalid",
          "route request inspection input is invalid");
  const auto semantic_digest = detail::RouteSelectionSemanticIntentDigest(intent);
  std::lock_guard lock(mutex_);
  RequireOpen();
  Transaction transaction(database_);
  auto result = InspectRouteSelectionRequestLocked(active_owner_epoch, intent, semantic_digest);
  transaction.Commit();
  return result;
}

std::optional<CCLCRouteSelectionRequestResultDTO>
SqliteCCLCJournal::Impl::ReadRouteSelectionRequestResult(const RouteSelectionIntentDTO& intent) {
  Require(intent.locally_valid(), "component_lifecycle_journal_query_invalid",
          "route result query is invalid");
  const auto digest = detail::RouteSelectionSemanticIntentDigest(intent);
  std::lock_guard lock(mutex_);
  RequireOpen();
  ReadTransaction transaction(database_);
  auto result = FindRouteSelectionRequestResultLocked(intent, digest);
  if (!result && UnstartedInFullNamespace(intent.request_context)) {
    result = CCLCRouteSelectionRequestResultDTO{RequestReplayStatusDTO::kRequestLedgerFull,
                                                std::nullopt, std::nullopt};
  }
  transaction.Commit();
  return result;
}

CCLCRouteSelectionRequestResultDTO SqliteCCLCJournal::Impl::AcceptOrReplayRouteSelection(
    const RequestOwnerEpoch& active_owner_epoch, const RouteSelectionIntentDTO& intent,
    const RouteSelectionAcceptedDTO& candidate_acceptance,
    const RouteSelectionStatusDTO& candidate_initial_status) {
  Require(!active_owner_epoch.empty() && intent.locally_valid(),
          "component_lifecycle_journal_record_invalid",
          "route request identity or semantic input is invalid");
  const auto semantic_digest = detail::RouteSelectionSemanticIntentDigest(intent);

  RefreshNativeApplyPreparationFailure();
  std::lock_guard lock(mutex_);
  RequireOpen();
  Transaction transaction(database_);
  auto existing = InspectRouteSelectionRequestLocked(active_owner_epoch, intent, semantic_digest);
  if (existing.has_value()) {
    transaction.Commit();
    return std::move(*existing);
  }
  const auto pending_apply = ReadUnsettledNativeApplyPlanLocked();
  if (pending_apply) {
    Fail("component_lifecycle_native_apply_unsettled",
         "native Apply " + pending_apply->apply_id.value() + "/" +
             std::to_string(pending_apply->apply_generation.value()) +
             " blocks new route-selection acceptance");
  }

  Require(
      candidate_acceptance.locally_valid() && candidate_initial_status.locally_valid() &&
          candidate_acceptance.replay_status == RequestReplayStatusDTO::kAcceptedNew &&
          candidate_initial_status.acceptance == candidate_acceptance &&
          candidate_initial_status.accepted_intent == intent &&
          candidate_initial_status.status_owner_epoch == active_owner_epoch &&
          candidate_initial_status.status_sequence == 1U &&
          candidate_initial_status.phase == RouteSelectionTransactionPhaseDTO::kAccepted &&
          !candidate_initial_status.accepted_fence.has_value() &&
          !candidate_initial_status.resulting_active_control_configuration.has_value() &&
          !candidate_initial_status.terminal_result.has_value() &&
          FitsSqliteInteger(candidate_acceptance.route_selection_transaction_generation.value()) &&
          FitsSqliteInteger(
              candidate_acceptance.candidate_active_control_configuration_generation.value()),
      "component_lifecycle_journal_record_invalid",
      "unseen route-selection acceptance candidate is invalid");
  const auto status_blob = detail::EncodeRouteSelectionStatus(candidate_initial_status);
  Require(intent.expected_admission_generation.has_value(), "component_lifecycle_claim_missing",
          "new route acceptance requires admission");
  std::optional<CellAdmissionConfigurationScopeDTO> recovery_configuration;
  if (intent.expected_active_control_configuration.presence ==
      ExpectedActiveControlConfigurationPresenceDTO::kPresent) {
    recovery_configuration = CellAdmissionConfigurationScopeDTO{
        intent.desired_configuration.control_domain_id,
        *intent.expected_active_control_configuration.active_control_configuration_id,
        *intent.expected_active_control_configuration.active_control_configuration_generation};
  }
  BindCellClaim(intent.request_context, candidate_acceptance.route_selection_transaction_id.value(),
                *intent.expected_admission_generation, semantic_digest, recovery_configuration);

  const auto active_count = ActiveTransactionCount();
  if (active_count >= kMaximumCCLCActiveTransactions) {
    Fail("request_queue_full", "CCLC active nonterminal transaction capacity is exhausted");
  }

  const auto highest =
      Nonnegative(ScalarInteger(database_,
                                "SELECT highest_route_selection_transaction_generation FROM "
                                "cclc_journal_metadata WHERE singleton=1"),
                  "highest route-selection transaction generation");
  Require(highest < static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) &&
              candidate_acceptance.route_selection_transaction_generation.value() == highest + 1U,
          "component_lifecycle_journal_generation_conflict",
          "route-selection transaction generation is not the next durable value");

  const auto predecessor_generation =
      candidate_acceptance.predecessor_active_control_configuration.has_value()
          ? candidate_acceptance.predecessor_active_control_configuration
                ->active_control_configuration_generation.value()
          : 0U;
  std::optional<std::uint64_t> durable_watermark;
  Statement active_configuration(
      database_,
      "SELECT active_control_configuration_generation FROM "
      "control_domain_active_configuration_watermarks WHERE control_domain_id=?");
  active_configuration.Bind(1, intent.desired_configuration.control_domain_id.value());
  if (active_configuration.Step()) {
    durable_watermark =
        Positive(active_configuration.Integer(0), "active-configuration generation watermark");
    Require(!active_configuration.Step(), "component_lifecycle_journal_corrupt",
            "active-configuration watermark is duplicated");
  } else {
    const auto watermark_count =
        Nonnegative(ScalarInteger(database_,
                                  "SELECT count(*) FROM "
                                  "control_domain_active_configuration_watermarks"),
                    "active-configuration watermark count");
    if (watermark_count >= kMaximumCCLCActiveConfigurationWatermarks) {
      Fail("component_lifecycle_journal_capacity_exhausted",
           "active-configuration watermark capacity is exhausted");
    }
  }

  bool bootstrap_predecessor = false;
  Statement current_configuration(
      database_,
      "SELECT configuration_blob FROM control_domain_active_configurations "
      "WHERE control_domain_id=?");
  current_configuration.Bind(1, intent.desired_configuration.control_domain_id.value());
  if (current_configuration.Step()) {
    Require(candidate_acceptance.predecessor_active_control_configuration.has_value() &&
                detail::DecodeActiveControlConfiguration(current_configuration.Blob(0)) ==
                    *candidate_acceptance.predecessor_active_control_configuration &&
                !current_configuration.Step(),
            "component_lifecycle_journal_generation_conflict",
            "route predecessor differs from the durable current publication");
    Require(durable_watermark.has_value() && *durable_watermark >= predecessor_generation,
            "component_lifecycle_journal_corrupt",
            "current active configuration is not covered by its durable watermark");
  } else if (candidate_acceptance.predecessor_active_control_configuration.has_value()) {
    Require(!durable_watermark.has_value(), "component_lifecycle_journal_generation_conflict",
            "a predecessor appeared after this domain already minted candidate generations");
    bootstrap_predecessor = true;
  }

  const auto generation_basis = durable_watermark.value_or(predecessor_generation);
  Require(generation_basis < static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) &&
              candidate_acceptance.candidate_active_control_configuration_generation.value() ==
                  generation_basis + 1U,
          "component_lifecycle_journal_generation_conflict",
          "candidate active-configuration generation is not the durable watermark successor");

  Require(
      candidate_acceptance.route_selection_transaction_id.value() ==
          "route-selection." +
              std::to_string(candidate_acceptance.route_selection_transaction_generation.value()),
      "component_lifecycle_journal_transaction_identity_reused",
      "route identity must name its exact immediate-successor generation");
  Statement guarded_identity(
      database_,
      "SELECT route_selection_transaction_generation FROM "
      "cclc_route_transaction_identity_guards WHERE route_selection_transaction_id=?");
  guarded_identity.Bind(1, candidate_acceptance.route_selection_transaction_id.value());
  Require(!guarded_identity.Step(), "component_lifecycle_journal_transaction_identity_reused",
          "route-selection transaction identity was already used");

  Statement insert_membership(database_,
                              "INSERT INTO cclc_request_memberships("
                              "owner_epoch,request_id,request_kind,semantic_request_digest,"
                              "retained_result) VALUES(?,?,'route_selection',?,1)");
  insert_membership.Bind(1, active_owner_epoch.value());
  insert_membership.Bind(2, intent.request_context.request_id.value());
  insert_membership.Bind(3, semantic_digest);
  Require(!insert_membership.Step(), "component_lifecycle_journal_corrupt",
          "route request membership insert returned an unexpected row");
  const auto membership_sequence = sqlite3_last_insert_rowid(database_);
  Require(membership_sequence > 0, "component_lifecycle_journal_corrupt",
          "route request membership sequence is invalid");

  Statement insert_identity_guard(
      database_,
      "INSERT INTO cclc_route_transaction_identity_guards("
      "route_selection_transaction_id,route_selection_transaction_generation) VALUES(?,?)");
  insert_identity_guard.Bind(1, candidate_acceptance.route_selection_transaction_id.value());
  insert_identity_guard.Bind(
      2, static_cast<std::int64_t>(
             candidate_acceptance.route_selection_transaction_generation.value()));
  Require(!insert_identity_guard.Step(), "component_lifecycle_journal_transaction_identity_reused",
          "route transaction identity or generation was already used");

  Statement insert(database_,
                   "INSERT INTO route_selection_transactions("
                   "membership_sequence,route_selection_transaction_id,"
                   "route_selection_transaction_generation,control_domain_id,"
                   "active_control_configuration_generation,status_sequence,accepted_at_ns,"
                   "latest_updated_at_ns,terminal,terminal_sequence,status_blob) "
                   "VALUES(?,?,?,?,?,?,?,?,0,NULL,?)");
  insert.Bind(1, membership_sequence);
  insert.Bind(2, candidate_acceptance.route_selection_transaction_id.value());
  insert.Bind(3, static_cast<std::int64_t>(
                     candidate_acceptance.route_selection_transaction_generation.value()));
  insert.Bind(4, intent.desired_configuration.control_domain_id.value());
  insert.Bind(5,
              static_cast<std::int64_t>(
                  candidate_acceptance.candidate_active_control_configuration_generation.value()));
  insert.Bind(6, static_cast<std::int64_t>(candidate_initial_status.status_sequence));
  insert.Bind(7, candidate_initial_status.accepted_at_ns);
  insert.Bind(8, candidate_initial_status.latest_updated_at_ns);
  insert.BindBlob(9, status_blob);
  Require(!insert.Step(), "component_lifecycle_journal_corrupt",
          "route-selection transaction insert returned an unexpected row");

  Statement update_metadata(database_,
                            "UPDATE cclc_journal_metadata SET "
                            "highest_route_selection_transaction_generation=? WHERE singleton=1");
  update_metadata.Bind(1, static_cast<std::int64_t>(
                              candidate_acceptance.route_selection_transaction_generation.value()));
  Require(!update_metadata.Step() && sqlite3_changes(database_) == 1,
          "component_lifecycle_journal_corrupt",
          "route-selection transaction watermark did not advance");

  Statement update_active_configuration(
      database_,
      "INSERT INTO control_domain_active_configuration_watermarks("
      "control_domain_id,active_control_configuration_generation) VALUES(?,?) "
      "ON CONFLICT(control_domain_id) DO UPDATE SET "
      "active_control_configuration_generation="
      "excluded.active_control_configuration_generation");
  update_active_configuration.Bind(1, intent.desired_configuration.control_domain_id.value());
  update_active_configuration.Bind(
      2, static_cast<std::int64_t>(
             candidate_acceptance.candidate_active_control_configuration_generation.value()));
  Require(!update_active_configuration.Step(), "component_lifecycle_journal_corrupt",
          "active-configuration watermark update returned an unexpected row");

  if (bootstrap_predecessor) {
    const auto& predecessor = *candidate_acceptance.predecessor_active_control_configuration;
    Statement insert_current(database_,
                             "INSERT INTO control_domain_active_configurations("
                             "control_domain_id,active_control_configuration_generation,"
                             "origin_route_selection_transaction_id,"
                             "origin_route_selection_transaction_generation,configuration_blob) "
                             "VALUES(?,?,NULL,NULL,?)");
    insert_current.Bind(1, predecessor.control_domain_id.value());
    insert_current.Bind(
        2, static_cast<std::int64_t>(predecessor.active_control_configuration_generation.value()));
    insert_current.BindBlob(3, detail::EncodeActiveControlConfiguration(predecessor));
    Require(!insert_current.Step(), "component_lifecycle_journal_corrupt",
            "bootstrap active-control-configuration insert returned an unexpected row");
  }

  CompactTerminalResults();
  transaction.Commit();
  return CCLCRouteSelectionRequestResultDTO{RequestReplayStatusDTO::kAcceptedNew,
                                            candidate_acceptance, candidate_initial_status};
}

std::uint64_t SqliteCCLCJournal::Impl::RecordEffectPrepared(
    const RequestOwnerEpoch& active_owner_epoch, const CCLCEffectAttemptDTO& prepared_attempt,
    const ComponentLifecycleStatusDTO& successor_status) {
  Require(!active_owner_epoch.empty() && prepared_attempt.locally_valid() &&
              prepared_attempt.state == CCLCEffectAttemptStateDTO::kPrepared &&
              successor_status.locally_valid() &&
              successor_status.status_owner_epoch == active_owner_epoch &&
              FitsSqliteInteger(prepared_attempt.effect_sequence) &&
              FitsSqliteInteger(successor_status.status_sequence),
          "component_lifecycle_journal_attempt_invalid",
          "CCLC prepared effect or successor status is invalid");
  const auto attempt_blob = detail::EncodeCCLCEffectAttempt(prepared_attempt);
  const auto status_blob = detail::EncodeComponentLifecycleStatus(successor_status);
  const auto status_digest = content_identity::sha256_digest(status_blob);
  const auto effect_identity = EffectIdentity(prepared_attempt);

  std::lock_guard lock(mutex_);
  RequireOpen();
  Transaction transaction(database_);
  Require(ActiveOwner(active_owner_epoch), "component_lifecycle_journal_owner_epoch_stale",
          "CCLC prepared-effect writer does not own the active epoch");

  Statement retained(database_,
                     "SELECT terminal,status_blob FROM lifecycle_transactions "
                     "WHERE lifecycle_transaction_id=?");
  retained.Bind(1, prepared_attempt.lifecycle_transaction_id.value());
  Require(retained.Step(), "component_lifecycle_journal_transaction_missing",
          "CCLC prepared effect transaction is not retained");
  const auto terminal = retained.Integer(0);
  const auto previous_blob = retained.Blob(1);
  const auto previous = detail::DecodeComponentLifecycleStatus(previous_blob);
  Require(!retained.Step(), "component_lifecycle_journal_corrupt",
          "CCLC prepared-effect transaction is duplicated");

  Statement existing(database_,
                     "SELECT attempt_sequence,lifecycle_transaction_id,effect_sequence,"
                     "effect_kind,prepared_status_digest,prepared_attempt_blob,"
                     "completed_attempt_blob "
                     "FROM cclc_effect_attempts WHERE effect_identity=?");
  existing.Bind(1, effect_identity);
  if (existing.Step()) {
    const auto row_sequence = Positive(existing.Integer(0), "CCLC effect-attempt row sequence");
    const bool exact =
        existing.Text(1) == prepared_attempt.lifecycle_transaction_id.value() &&
        Positive(existing.Integer(2), "CCLC effect sequence") == prepared_attempt.effect_sequence &&
        existing.Text(3) == EffectKindName(prepared_attempt.effect_kind) &&
        existing.Text(4) == status_digest && existing.Blob(5) == attempt_blob && existing.IsNull(6);
    Require(!existing.Step() && exact, "component_lifecycle_journal_attempt_conflict",
            "CCLC effect identity was already prepared differently");
    transaction.Commit();
    return row_sequence;
  }

  Statement unresolved(database_,
                       "SELECT count(*) FROM cclc_effect_attempts "
                       "WHERE lifecycle_transaction_id=? AND completed_attempt_blob IS NULL");
  unresolved.Bind(1, prepared_attempt.lifecycle_transaction_id.value());
  Require(unresolved.Step() && unresolved.Integer(0) == 0 && !unresolved.Step(),
          "component_lifecycle_journal_attempt_unresolved",
          "CCLC transaction already has an unresolved external effect");

  Require(terminal == 0 && previous.status_owner_epoch == active_owner_epoch &&
              successor_status.acceptance.lifecycle_transaction_id ==
                  prepared_attempt.lifecycle_transaction_id &&
              successor_status.acceptance.lifecycle_transaction_generation ==
                  prepared_attempt.lifecycle_transaction_generation &&
              StatusReplacementIsMonotonic(previous, successor_status) &&
              !Terminal(successor_status),
          "component_lifecycle_journal_status_regressed",
          "prepared effect successor status is not monotonic and nonterminal");
  switch (prepared_attempt.effect_kind) {
    case CCLCEffectKindDTO::kBrandLifecyclePrimitive: {
      const auto& intent = *prepared_attempt.lifecycle_primitive_intent;
      const bool stateless_disable =
          successor_status.accepted_intent.intent == ComponentLifecycleIntentKindDTO::kDisable &&
          !previous.current_state.has_value() && !successor_status.current_state.has_value();
      Require(!stateless_disable &&
                  successor_status.accepted_intent.controllable_component_id ==
                      intent.controllable_component_id &&
                  std::any_of(successor_status.domains.begin(), successor_status.domains.end(),
                              [&](const auto& domain) {
                                return domain.control_domain_id == intent.control_domain_id;
                              }),
              "component_lifecycle_journal_attempt_invalid",
              "brand lifecycle effect is outside the accepted component scope");
      break;
    }
  }

  Statement highest_effect_statement(
      database_,
      "SELECT coalesce(max(effect_sequence),0) FROM cclc_effect_attempts "
      "WHERE lifecycle_transaction_id=?");
  highest_effect_statement.Bind(1, prepared_attempt.lifecycle_transaction_id.value());
  Require(highest_effect_statement.Step(), "component_lifecycle_journal_corrupt",
          "highest CCLC effect sequence is missing");
  const auto highest_effect =
      Nonnegative(highest_effect_statement.Integer(0), "highest CCLC effect sequence");
  Require(
      !highest_effect_statement.Step() &&
          highest_effect < static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) &&
          prepared_attempt.effect_sequence == highest_effect + 1U,
      "component_lifecycle_journal_attempt_conflict",
      "CCLC effect sequence is not the next durable value");

  Statement history_bytes(database_,
                          "SELECT coalesce(sum(length(prepared_attempt_blob)+"
                          "coalesce(length(completed_attempt_blob),0)),0) "
                          "FROM cclc_effect_attempts WHERE lifecycle_transaction_id=?");
  history_bytes.Bind(1, prepared_attempt.lifecycle_transaction_id.value());
  Require(history_bytes.Step(), "component_lifecycle_journal_corrupt",
          "CCLC effect-history byte count is missing");
  const auto retained_bytes = Nonnegative(history_bytes.Integer(0), "CCLC effect-history bytes");
  Require(!history_bytes.Step() &&
              retained_bytes <=
                  detail::kMaximumComponentLifecycleJournalRecordBytes - attempt_blob.size(),
          "component_lifecycle_journal_capacity_exhausted",
          "CCLC transaction effect history would exceed its byte bound");

  Statement insert(database_,
                   "INSERT INTO cclc_effect_attempts("
                   "lifecycle_transaction_id,effect_sequence,effect_kind,effect_identity,"
                   "prepared_owner_epoch,prepared_status_sequence,prepared_status_digest,"
                   "prepared_attempt_blob) VALUES(?,?,?,?,?,?,?,?)");
  insert.Bind(1, prepared_attempt.lifecycle_transaction_id.value());
  insert.Bind(2, static_cast<std::int64_t>(prepared_attempt.effect_sequence));
  insert.Bind(3, EffectKindName(prepared_attempt.effect_kind));
  insert.Bind(4, effect_identity);
  insert.Bind(5, active_owner_epoch.value());
  insert.Bind(6, static_cast<std::int64_t>(successor_status.status_sequence));
  insert.Bind(7, status_digest);
  insert.BindBlob(8, attempt_blob);
  Require(!insert.Step(), "component_lifecycle_journal_attempt_conflict",
          "CCLC effect preparation could not reserve one unique row");
  const auto row_sequence = sqlite3_last_insert_rowid(database_);
  Require(row_sequence > 0, "component_lifecycle_journal_corrupt",
          "CCLC effect-attempt row sequence is invalid");

  Statement update_status(database_,
                          "UPDATE lifecycle_transactions SET status_sequence=?,"
                          "latest_updated_at_ns=?,terminal=0,status_blob=? "
                          "WHERE lifecycle_transaction_id=?");
  update_status.Bind(1, static_cast<std::int64_t>(successor_status.status_sequence));
  update_status.Bind(2, successor_status.latest_updated_at_ns);
  update_status.BindBlob(3, status_blob);
  update_status.Bind(4, prepared_attempt.lifecycle_transaction_id.value());
  Require(!update_status.Step() && sqlite3_changes(database_) == 1,
          "component_lifecycle_journal_corrupt",
          "prepared CCLC effect status did not replace one transaction");
  RetainCurrentComponentLifecycleFact(successor_status);
  transaction.Commit();
  return static_cast<std::uint64_t>(row_sequence);
}

std::uint64_t SqliteCCLCJournal::Impl::RecordEffectCompleted(
    const RequestOwnerEpoch& active_owner_epoch, const CCLCEffectAttemptDTO& completed_attempt,
    const ComponentLifecycleStatusDTO& successor_status) {
  Require(!active_owner_epoch.empty() && completed_attempt.locally_valid() &&
              completed_attempt.state == CCLCEffectAttemptStateDTO::kCompleted &&
              successor_status.locally_valid() &&
              successor_status.status_owner_epoch == active_owner_epoch &&
              FitsSqliteInteger(completed_attempt.effect_sequence) &&
              FitsSqliteInteger(successor_status.status_sequence),
          "component_lifecycle_journal_attempt_invalid",
          "CCLC completed effect or successor status is invalid");
  const auto completed_blob = detail::EncodeCCLCEffectAttempt(completed_attempt);
  const auto prepared_blob = detail::EncodeCCLCEffectAttempt(PreparedForm(completed_attempt));
  const auto status_blob = detail::EncodeComponentLifecycleStatus(successor_status);
  const auto status_digest = content_identity::sha256_digest(status_blob);
  const auto effect_identity = EffectIdentity(completed_attempt);

  std::lock_guard lock(mutex_);
  RequireOpen();
  Transaction transaction(database_);
  Require(ActiveOwner(active_owner_epoch), "component_lifecycle_journal_owner_epoch_stale",
          "CCLC completed-effect writer does not own the active epoch");

  Statement attempt(database_,
                    "SELECT attempt_sequence,prepared_attempt_blob,completed_attempt_blob,"
                    "completed_status_digest "
                    "FROM cclc_effect_attempts WHERE lifecycle_transaction_id=? AND "
                    "effect_sequence=? AND effect_identity=? AND effect_kind=?");
  attempt.Bind(1, completed_attempt.lifecycle_transaction_id.value());
  attempt.Bind(2, static_cast<std::int64_t>(completed_attempt.effect_sequence));
  attempt.Bind(3, effect_identity);
  attempt.Bind(4, EffectKindName(completed_attempt.effect_kind));
  Require(attempt.Step(), "component_lifecycle_journal_attempt_missing",
          "completed CCLC effect has no matching prepared row");
  const auto row_sequence = Positive(attempt.Integer(0), "CCLC effect-attempt row sequence");
  const auto retained_prepared = attempt.Blob(1);
  const bool already_completed = !attempt.IsNull(2);
  std::string retained_completed;
  std::string retained_status_digest;
  if (already_completed) {
    retained_completed = attempt.Blob(2);
    Require(!attempt.IsNull(3), "component_lifecycle_journal_corrupt",
            "completed CCLC effect has no successor-status digest");
    retained_status_digest = attempt.Text(3);
  }
  Require(!attempt.Step() && retained_prepared == prepared_blob,
          "component_lifecycle_journal_attempt_conflict",
          "completed CCLC effect conflicts with its prepared request");
  if (already_completed) {
    Require(retained_completed == completed_blob && retained_status_digest == status_digest,
            "component_lifecycle_journal_attempt_conflict",
            "CCLC effect already completed with a different result or status");
    transaction.Commit();
    return row_sequence;
  }

  Statement retained(database_,
                     "SELECT terminal,status_blob FROM lifecycle_transactions "
                     "WHERE lifecycle_transaction_id=?");
  retained.Bind(1, completed_attempt.lifecycle_transaction_id.value());
  Require(retained.Step(), "component_lifecycle_journal_transaction_missing",
          "completed CCLC effect transaction is not retained");
  const auto previous_terminal = retained.Integer(0);
  const auto previous = detail::DecodeComponentLifecycleStatus(retained.Blob(1));
  Require(!retained.Step(), "component_lifecycle_journal_corrupt",
          "completed CCLC effect transaction is duplicated");
  Require(previous_terminal == 0 && StatusReplacementIsMonotonic(previous, successor_status),
          "component_lifecycle_journal_status_regressed",
          "completed effect successor status is not monotonic");

  switch (completed_attempt.effect_kind) {
    case CCLCEffectKindDTO::kBrandLifecyclePrimitive: {
      const auto& result = *completed_attempt.lifecycle_primitive_result;
      Require(std::any_of(successor_status.domains.begin(), successor_status.domains.end(),
                          [&](const auto& domain) {
                            return std::find(domain.primitive_attempts.begin(),
                                             domain.primitive_attempts.end(),
                                             result) != domain.primitive_attempts.end();
                          }),
              "component_lifecycle_journal_attempt_conflict",
              "completed brand effect is absent from its successor status");
      break;
    }
  }

  Statement history_bytes(database_,
                          "SELECT coalesce(sum(length(prepared_attempt_blob)+"
                          "coalesce(length(completed_attempt_blob),0)),0) "
                          "FROM cclc_effect_attempts WHERE lifecycle_transaction_id=?");
  history_bytes.Bind(1, completed_attempt.lifecycle_transaction_id.value());
  Require(history_bytes.Step(), "component_lifecycle_journal_corrupt",
          "CCLC effect-history byte count is missing");
  const auto retained_bytes = Nonnegative(history_bytes.Integer(0), "CCLC effect-history bytes");
  Require(!history_bytes.Step() &&
              retained_bytes <=
                  detail::kMaximumComponentLifecycleJournalRecordBytes - completed_blob.size(),
          "component_lifecycle_journal_capacity_exhausted",
          "CCLC transaction effect history would exceed its byte bound");

  if (Terminal(successor_status)) {
    CompactTerminalResults();  // The unreleased claim still owns this result.
  }

  Statement update_attempt(database_,
                           "UPDATE cclc_effect_attempts SET completed_owner_epoch=?,"
                           "completed_status_sequence=?,completed_status_digest=?,"
                           "completed_attempt_blob=? "
                           "WHERE attempt_sequence=?");
  update_attempt.Bind(1, active_owner_epoch.value());
  update_attempt.Bind(2, static_cast<std::int64_t>(successor_status.status_sequence));
  update_attempt.Bind(3, status_digest);
  update_attempt.BindBlob(4, completed_blob);
  update_attempt.Bind(5, static_cast<std::int64_t>(row_sequence));
  Require(!update_attempt.Step() && sqlite3_changes(database_) == 1,
          "component_lifecycle_journal_corrupt",
          "completed CCLC effect did not update one prepared row");

  if (Terminal(successor_status)) {
    Require(TerminalStatusIsQuiescent(successor_status),
            "component_lifecycle_journal_attempt_unresolved",
            "terminal lifecycle status retains a reconciliation obligation");
    Statement unresolved(database_,
                         "SELECT count(*) FROM cclc_effect_attempts "
                         "WHERE lifecycle_transaction_id=? AND completed_attempt_blob IS NULL");
    unresolved.Bind(1, completed_attempt.lifecycle_transaction_id.value());
    Require(unresolved.Step() && unresolved.Integer(0) == 0 && !unresolved.Step(),
            "component_lifecycle_journal_attempt_unresolved",
            "terminal lifecycle status has an unresolved effect attempt");
  }

  const auto terminal_sequence = Terminal(successor_status)
                                     ? std::optional<std::uint64_t>(MintTerminalSequence())
                                     : std::nullopt;

  Statement update_status(database_,
                          "UPDATE lifecycle_transactions SET status_sequence=?,"
                          "latest_updated_at_ns=?,terminal=?,terminal_sequence=?,status_blob=? "
                          "WHERE lifecycle_transaction_id=?");
  update_status.Bind(1, static_cast<std::int64_t>(successor_status.status_sequence));
  update_status.Bind(2, successor_status.latest_updated_at_ns);
  update_status.Bind(3, Terminal(successor_status) ? 1 : 0);
  if (terminal_sequence.has_value()) {
    update_status.Bind(4, static_cast<std::int64_t>(*terminal_sequence));
  } else {
    update_status.BindNull(4);
  }
  update_status.BindBlob(5, status_blob);
  update_status.Bind(6, completed_attempt.lifecycle_transaction_id.value());
  Require(!update_status.Step() && sqlite3_changes(database_) == 1,
          "component_lifecycle_journal_corrupt",
          "completed CCLC effect status did not replace one transaction");
  RetainCurrentComponentLifecycleFact(successor_status);
  CompactTerminalResults();
  transaction.Commit();
  return row_sequence;
}

std::uint64_t SqliteCCLCJournal::Impl::RecordLifecycleStatus(
    const RequestOwnerEpoch& active_owner_epoch, const ComponentLifecycleStatusDTO& status,
    const CellAdmissionClaimReleaseDTO* release) {
  Require(!active_owner_epoch.empty() && status.locally_valid() &&
              status.status_owner_epoch == active_owner_epoch &&
              FitsSqliteInteger(status.status_sequence),
          "component_lifecycle_journal_record_invalid",
          "lifecycle status is invalid at journal ingress");
  const auto encoded = detail::EncodeComponentLifecycleStatus(status);
  std::lock_guard lock(mutex_);
  RequireOpen();
  Transaction transaction(database_);
  Require(ActiveOwner(active_owner_epoch), "component_lifecycle_journal_owner_epoch_stale",
          "lifecycle status writer does not own the active epoch");

  Statement retained(database_,
                     "SELECT status_blob FROM lifecycle_transactions "
                     "WHERE lifecycle_transaction_id=?");
  retained.Bind(1, status.acceptance.lifecycle_transaction_id.value());
  Require(retained.Step(), "component_lifecycle_journal_transaction_missing",
          "lifecycle transaction is not retained");
  const auto previous = detail::DecodeComponentLifecycleStatus(retained.Blob(0));
  Require(!retained.Step(), "component_lifecycle_journal_corrupt",
          "lifecycle transaction identity is duplicated");

  Statement unresolved(database_,
                       "SELECT count(*) FROM cclc_effect_attempts "
                       "WHERE lifecycle_transaction_id=? AND completed_attempt_blob IS NULL");
  unresolved.Bind(1, status.acceptance.lifecycle_transaction_id.value());
  Require(unresolved.Step(), "component_lifecycle_journal_corrupt",
          "lifecycle unresolved-effect count is missing");
  const bool has_unresolved_prepared_effect = unresolved.Integer(0) != 0;
  Require(!unresolved.Step(), "component_lifecycle_journal_corrupt",
          "lifecycle unresolved-effect count is duplicated");

  Require(OwnerReplacementStatusIsMonotonic(previous, status, has_unresolved_prepared_effect),
          "component_lifecycle_journal_status_regressed",
          "lifecycle status replacement is not monotonic");

  for (const auto& domain : status.domains) {
    for (const auto& result : domain.primitive_attempts) {
      Statement attempt(database_,
                        "SELECT lifecycle_transaction_id,completed_attempt_blob FROM "
                        "cclc_effect_attempts WHERE effect_identity=? AND "
                        "effect_kind='brand_lifecycle_primitive'");
      attempt.Bind(1, BrandEffectIdentity(status.acceptance.lifecycle_transaction_id,
                                          result.lifecycle_primitive_attempt_id));
      Require(attempt.Step() &&
                  attempt.Text(0) == status.acceptance.lifecycle_transaction_id.value() &&
                  !attempt.IsNull(1) &&
                  detail::DecodeCCLCEffectAttempt(attempt.Blob(1)).lifecycle_primitive_result ==
                      result &&
                  !attempt.Step(),
              "component_lifecycle_journal_attempt_conflict",
              "lifecycle status contains an unprepared or conflicting attempt");
    }
  }
  if (Terminal(status)) {
    Require(TerminalStatusIsQuiescent(status), "component_lifecycle_journal_attempt_unresolved",
            "terminal lifecycle status retains a reconciliation obligation");
    Require(!has_unresolved_prepared_effect, "component_lifecycle_journal_attempt_unresolved",
            "terminal lifecycle status has an unresolved effect attempt");
    Statement history_bytes(database_,
                            "SELECT coalesce(sum(length(prepared_attempt_blob)+"
                            "coalesce(length(completed_attempt_blob),0)),0) "
                            "FROM cclc_effect_attempts WHERE lifecycle_transaction_id=?");
    history_bytes.Bind(1, status.acceptance.lifecycle_transaction_id.value());
    Require(history_bytes.Step(), "component_lifecycle_journal_corrupt",
            "terminal lifecycle effect-history byte count is missing");
    static_cast<void>(
        Nonnegative(history_bytes.Integer(0), "terminal lifecycle effect-history bytes"));
    Require(!history_bytes.Step(), "component_lifecycle_journal_corrupt",
            "terminal lifecycle effect-history byte count is duplicated");
    CompactTerminalResults();  // The unreleased claim still owns this result.
  }

  const auto terminal_sequence =
      Terminal(status) ? std::optional<std::uint64_t>(MintTerminalSequence()) : std::nullopt;

  Statement update(database_,
                   "UPDATE lifecycle_transactions SET status_sequence=?,"
                   "latest_updated_at_ns=?,terminal=?,terminal_sequence=?,status_blob=? "
                   "WHERE lifecycle_transaction_id=?");
  update.Bind(1, static_cast<std::int64_t>(status.status_sequence));
  update.Bind(2, status.latest_updated_at_ns);
  update.Bind(3, Terminal(status) ? 1 : 0);
  if (terminal_sequence.has_value()) {
    update.Bind(4, static_cast<std::int64_t>(*terminal_sequence));
  } else {
    update.BindNull(4);
  }
  update.BindBlob(5, encoded);
  update.Bind(6, status.acceptance.lifecycle_transaction_id.value());
  Require(!update.Step() && sqlite3_changes(database_) == 1, "component_lifecycle_journal_corrupt",
          "lifecycle status update did not affect exactly one row");
  if (release != nullptr) {
    Require(Terminal(status), "component_lifecycle_claim_release_invalid",
            "release requires terminal status");
    RetainCellClaimRelease(*release, status.acceptance.lifecycle_transaction_id.value(),
                           detail::ComponentLifecycleStatusReleasePreconditionDigest(status));
  }
  RetainCurrentComponentLifecycleFact(status);
  CompactTerminalResults();
  transaction.Commit();
  return status.status_sequence;
}

void SqliteCCLCJournal::Impl::RetainCellClaimRelease(const CellAdmissionClaimReleaseDTO& release,
                                                     std::string_view transaction_id,
                                                     std::string_view terminal_digest) {
  Require(ValidateCellAdmissionClaimRelease(release).ok() &&
              release.owner_role == CellAdmissionOwnerRoleDTO::kComponentLifecycleOwner &&
              release.owner_transaction_id == transaction_id &&
              release.settlement.settlement_kind ==
                  CellAdmissionSettlementKindDTO::kTerminalEffectsSettled &&
              release.settlement.evidence_digest.value() == terminal_digest,
          "component_lifecycle_claim_release_invalid", "terminal CCLC claim release is invalid");
  Statement retain(database_,
                   "UPDATE cclc_lifecycle_cell_claims SET release_kind=?,release_evidence_digest=? "
                   "WHERE lifecycle_transaction_id=? AND owner_epoch=? AND request_id=? AND "
                   "request_payload_digest=? "
                   "AND claim_generation=? AND acquired_admission_generation=? "
                   "AND lifecycle_accepted=1 AND release_kind IS NULL");
  retain.Bind(1, static_cast<std::int64_t>(release.settlement.settlement_kind));
  retain.Bind(2, terminal_digest);
  retain.Bind(3, transaction_id);
  retain.Bind(4, release.owner_epoch.value());
  retain.Bind(5, release.request_id.value());
  retain.Bind(6, release.request_payload_digest.value());
  retain.Bind(7, static_cast<std::int64_t>(release.claim_generation.value()));
  retain.Bind(8, static_cast<std::int64_t>(release.acquired_admission_generation.value()));
  Require(!retain.Step() && sqlite3_changes(database_) == 1,
          "component_lifecycle_claim_release_conflict",
          "terminal CCLC status has no exact held claim");
}

void SqliteCCLCJournal::Impl::RecordCellClaimReleaseReceipt(
    const RequestOwnerEpoch& active_owner_epoch, const CellAdmissionClaimReleaseDTO& release,
    const CellAdmissionRecoveryReceiptDTO& receipt) {
  const bool released_exact =
      receipt.status ==
          CellAdmissionRecoveryReceiptStatusDTO::kReleasedExact &&
      receipt.matched_release == release;
  const bool reconciled_exact =
      receipt.status ==
          CellAdmissionRecoveryReceiptStatusDTO::kReconciledExact &&
      receipt.matched_claim.has_value() &&
      receipt.matched_reconcile_settlement.has_value() &&
      receipt.matched_reconcile_settlement->predecessor_claim ==
          *receipt.matched_claim &&
      receipt.matched_claim->intent.owner_role == release.owner_role &&
      receipt.matched_claim->intent.owner_epoch == release.owner_epoch &&
      receipt.matched_claim->intent.owner_transaction_id ==
          release.owner_transaction_id &&
      receipt.matched_claim->intent.request_id == release.request_id &&
      receipt.matched_claim->intent.request_payload_digest ==
          release.request_payload_digest &&
      receipt.matched_claim->acquired_admission_generation ==
          release.acquired_admission_generation &&
      receipt.matched_claim->claim_generation == release.claim_generation;
  Require(ValidateCellAdmissionClaimRelease(release).ok() &&
              ValidateCellAdmissionRecoveryReceipt(receipt).ok() &&
              (released_exact || reconciled_exact),
          "component_lifecycle_claim_receipt_invalid",
          "lifecycle release receipt is not exact");
  std::lock_guard lock(mutex_);
  RequireOpen();
  Transaction transaction(database_);
  Require(ActiveOwner(active_owner_epoch),
          "component_lifecycle_journal_owner_epoch_stale",
          "lifecycle claim receipt writer does not own the active epoch");
  Statement update(
      database_,
      "UPDATE cclc_lifecycle_cell_claims SET release_receipt_recorded=1 "
      "WHERE lifecycle_transaction_id=? AND owner_epoch=? AND request_id=? "
      "AND release_kind=? AND release_evidence_digest=? "
      "AND release_receipt_recorded=0");
  update.Bind(1, release.owner_transaction_id);
  update.Bind(2, release.owner_epoch.value());
  update.Bind(3, release.request_id.value());
  update.Bind(4,
              static_cast<std::int64_t>(release.settlement.settlement_kind));
  update.Bind(5, release.settlement.evidence_digest.value());
  Require(!update.Step() && sqlite3_changes(database_) == 1,
          "component_lifecycle_claim_receipt_conflict",
          "lifecycle release receipt has no matching durable intent");
  Statement erase_settled(
      database_,
      "DELETE FROM cclc_lifecycle_cell_claims WHERE lifecycle_transaction_id=? "
      "AND release_receipt_recorded=1");
  erase_settled.Bind(1, release.owner_transaction_id);
  Require(!erase_settled.Step() && sqlite3_changes(database_) == 1,
          "component_lifecycle_journal_corrupt",
          "settled lifecycle claim retirement did not affect exactly one row");
  CompactTerminalResults();
  transaction.Commit();
}

void SqliteCCLCJournal::Impl::RecordUnacceptedCellClaimRelease(
    const RequestOwnerEpoch& active_owner_epoch, const CellAdmissionClaimReleaseDTO& release) {
  Require(ValidateCellAdmissionClaimRelease(release).ok() &&
              release.owner_role == CellAdmissionOwnerRoleDTO::kComponentLifecycleOwner &&
              release.settlement.evidence_digest == release.request_payload_digest &&
              release.settlement.settlement_kind == CellAdmissionSettlementKindDTO::kProvenNoEffect,
          "component_lifecycle_claim_release_invalid",
          "unaccepted lifecycle claim release is invalid");
  std::lock_guard lock(mutex_);
  RequireOpen();
  Transaction transaction(database_);
  RecordUnacceptedCellClaimReleaseLocked(active_owner_epoch, release);
  transaction.Commit();
}

void SqliteCCLCJournal::Impl::RecordUnacceptedCellClaimReleaseLocked(
    const RequestOwnerEpoch& active_owner_epoch, const CellAdmissionClaimReleaseDTO& release) {
  Require(ActiveOwner(active_owner_epoch),
          "component_lifecycle_journal_owner_epoch_stale",
          "claim recovery writer does not own the active epoch");
  Statement update(database_,
                   "UPDATE cclc_lifecycle_cell_claims SET release_kind=?,"
                   "release_evidence_digest=? WHERE lifecycle_transaction_id=? "
                   "AND claim_generation=? AND acquired_admission_generation=? "
                   "AND lifecycle_accepted=0 "
                   "AND owner_epoch=? AND request_id=? AND request_payload_digest=? "
                   "AND (release_kind IS NULL OR (release_kind=? AND release_evidence_digest=?))");
  update.Bind(1,
              static_cast<std::int64_t>(release.settlement.settlement_kind));
  update.Bind(2, release.settlement.evidence_digest.value());
  update.Bind(3, release.owner_transaction_id);
  update.Bind(4,
              static_cast<std::int64_t>(release.claim_generation.value()));
  update.Bind(
      5, static_cast<std::int64_t>(
             release.acquired_admission_generation.value()));
  update.Bind(6, release.owner_epoch.value());
  update.Bind(7, release.request_id.value());
  update.Bind(8, release.request_payload_digest.value());
  update.Bind(9, static_cast<std::int64_t>(release.settlement.settlement_kind));
  update.Bind(10, release.settlement.evidence_digest.value());
  Require(!update.Step() && sqlite3_changes(database_) == 1,
          "component_lifecycle_claim_release_conflict",
          "unaccepted lifecycle claim release has no exact claim");
  // Retain the no-admission outcome before retiring the claim on receipt.
  // The original request digest is already part of this exact claim; no
  // second intent/receipt store or reconstructed command is needed.
  RetainRecoveredNoAdmission(release.owner_epoch, release.owner_transaction_id, release.request_id,
                             ReadOriginalRequestDigestLocked(release.owner_transaction_id,
                                                             release.request_payload_digest));
}

void SqliteCCLCJournal::Impl::RecordCellClaimNotAcquired(
    const RequestOwnerEpoch& active_owner_epoch, const CellAdmissionClaimIntentDTO& intent) {
  Require(ValidCCLCClaimIntent(intent), "component_lifecycle_claim_invalid",
          "lifecycle claim attempt is invalid");
  std::lock_guard lock(mutex_);
  RequireOpen();
  Transaction transaction(database_);
  Require(ActiveOwner(active_owner_epoch),
          "component_lifecycle_journal_owner_epoch_stale",
          "claim recovery writer does not own the active epoch");
  // Keep the claim's old namespace pinned while inserting the result. The
  // exact DELETE below validates the attempt in this same transaction.
  const bool selected = intent.owner_transaction_id.starts_with("selected-lifecycle.");
  const auto recovery =
      selected ? ReadClaimRecoveryLocked(intent) : RecoveryConfigurationBytes(intent);
  const auto original_digest = selected ? OriginalRequestDigestFromRecovery(intent, recovery)
                                        : std::optional<ContentDigest>{};
  RetainRecoveredNoAdmission(intent.owner_epoch, intent.owner_transaction_id, intent.request_id,
                             original_digest.value_or(intent.request_payload_digest));
  Statement update(database_,
                   "DELETE FROM cclc_lifecycle_cell_claims "
                   "WHERE lifecycle_transaction_id=? AND claim_generation IS NULL "
                   "AND lifecycle_accepted=0 AND release_receipt_recorded=0 "
                   "AND owner_epoch=? AND request_id=? AND request_payload_digest=? "
                   "AND expected_admission_generation=? AND program_node_evidence IS ? AND "
                   "recovery_configuration IS ?");
  update.Bind(1, intent.owner_transaction_id);
  update.Bind(2, intent.owner_epoch.value());
  update.Bind(3, intent.request_id.value());
  update.Bind(4, intent.request_payload_digest.value());
  update.Bind(5, static_cast<std::int64_t>(intent.expected_admission_generation.value()));
  if (intent.program_node_evidence) {
    update.BindBlob(6, detail::EncodeProgramNodeAdmissionEvidence(*intent.program_node_evidence));
  } else {
    update.BindNull(6);
  }
  if (recovery)
    update.BindBlob(7, *recovery);
  else
    update.BindNull(7);
  Require(!update.Step() && sqlite3_changes(database_) == 1,
          "component_lifecycle_claim_receipt_conflict",
          "not-acquired receipt has no exact durable attempt");
  CompactTerminalResults();
  transaction.Commit();
}

std::optional<ComponentLifecycleStatusDTO> SqliteCCLCJournal::Impl::LoadRetainedLifecycleStatus(
    const LifecycleTransactionId& lifecycle_transaction_id) {
  Require(!lifecycle_transaction_id.empty(), "component_lifecycle_journal_query_invalid",
          "lifecycle transaction query identity is empty");
  std::lock_guard lock(mutex_);
  RequireOpen();
  Statement retained(database_,
                     "SELECT status_blob FROM lifecycle_transactions "
                     "WHERE lifecycle_transaction_id=?");
  retained.Bind(1, lifecycle_transaction_id.value());
  if (!retained.Step()) {
    return std::nullopt;
  }
  auto status = detail::DecodeComponentLifecycleStatus(retained.Blob(0));
  Require(!retained.Step(), "component_lifecycle_journal_corrupt",
          "lifecycle transaction identity is duplicated");
  return status;
}

ComponentLifecycleCurrentFactReadbackDTO SqliteCCLCJournal::Impl::LoadCurrentComponentLifecycleFact(
    const ControllableComponentId& controllable_component_id) {
  Require(!controllable_component_id.empty() &&
              controllable_component_id.value().size() <= kMaxLifecycleIdentityBytes,
          "component_lifecycle_journal_query_invalid",
          "current component lifecycle query identity is invalid");
  std::lock_guard lock(mutex_);
  RequireOpen();

  std::optional<ComponentLifecycleGeneration> durable_generation;
  Statement watermark(database_,
                      "SELECT component_lifecycle_generation FROM "
                      "lifecycle_component_watermarks WHERE controllable_component_id=?");
  watermark.Bind(1, controllable_component_id.value());
  if (watermark.Step()) {
    durable_generation = ComponentLifecycleGeneration(
        Positive(watermark.Integer(0), "component lifecycle generation watermark"));
    Require(!watermark.Step(), "component_lifecycle_journal_corrupt",
            "component lifecycle watermark is duplicated");
  }

  std::optional<ComponentLifecycleStateDTO> current_state;
  Statement fact(database_,
                 "SELECT component_lifecycle_generation,component_lifecycle_state "
                 "FROM lifecycle_component_current_facts WHERE controllable_component_id=?");
  fact.Bind(1, controllable_component_id.value());
  if (fact.Step()) {
    const auto fact_generation =
        Positive(fact.Integer(0), "current component lifecycle generation");
    const auto raw_state = fact.Integer(1);
    const auto state = static_cast<ComponentLifecycleStateDTO>(raw_state);
    Require(durable_generation.has_value() && durable_generation->value() == fact_generation &&
                raw_state >= 0 && raw_state <= 7 && !to_string(state).empty() && !fact.Step(),
            "component_lifecycle_journal_corrupt",
            "current component lifecycle fact is stale or invalid");
    current_state = state;
  }

  ComponentLifecycleCurrentFactReadbackDTO result{controllable_component_id, durable_generation,
                                                  current_state};
  Require(result.locally_valid(), "component_lifecycle_journal_corrupt",
          "current component lifecycle readback is invalid");
  return result;
}

CCLCCurrentFactsReadbackDTO SqliteCCLCJournal::Impl::LoadCurrentSnapshotFacts(
    const std::vector<ControllableComponentId>& canonical_controllable_component_ids,
    const std::vector<ControlDomainId>& canonical_control_domain_ids) {
  Require(!canonical_controllable_component_ids.empty() &&
              canonical_controllable_component_ids.size() <= kMaxLifecycleFacts &&
              std::is_sorted(canonical_controllable_component_ids.begin(),
                             canonical_controllable_component_ids.end()) &&
              std::adjacent_find(canonical_controllable_component_ids.begin(),
                                 canonical_controllable_component_ids.end()) ==
                  canonical_controllable_component_ids.end() &&
              std::all_of(canonical_controllable_component_ids.begin(),
                          canonical_controllable_component_ids.end(),
                          [](const ControllableComponentId& component) {
                            return !component.empty() &&
                                   component.value().size() <= kMaxLifecycleIdentityBytes;
                          }) &&
              !canonical_control_domain_ids.empty() &&
              canonical_control_domain_ids.size() <= kMaxLifecycleResolvedDomains &&
              std::is_sorted(canonical_control_domain_ids.begin(),
                             canonical_control_domain_ids.end()) &&
              std::adjacent_find(canonical_control_domain_ids.begin(),
                                 canonical_control_domain_ids.end()) ==
                  canonical_control_domain_ids.end() &&
              std::all_of(canonical_control_domain_ids.begin(), canonical_control_domain_ids.end(),
                          [](const ControlDomainId& domain) {
                            return !domain.empty() &&
                                   domain.value().size() <= kMaxLifecycleIdentityBytes;
                          }),
          "component_lifecycle_journal_query_invalid",
          "current component snapshot fact query is invalid");
  std::lock_guard lock(mutex_);
  RequireOpen();
  ReadTransaction transaction(database_);

  CCLCCurrentFactsReadbackDTO result{{}, {}};
  result.components.reserve(canonical_controllable_component_ids.size());
  for (const auto& controllable_component_id : canonical_controllable_component_ids) {
    std::optional<ComponentLifecycleGeneration> durable_generation;
    Statement watermark(database_,
                        "SELECT component_lifecycle_generation FROM "
                        "lifecycle_component_watermarks WHERE "
                        "controllable_component_id=?");
    watermark.Bind(1, controllable_component_id.value());
    if (watermark.Step()) {
      durable_generation = ComponentLifecycleGeneration(
          Positive(watermark.Integer(0), "component lifecycle generation watermark"));
      Require(!watermark.Step(), "component_lifecycle_journal_corrupt",
              "component lifecycle watermark is duplicated");
    }

    std::optional<ComponentLifecycleStateDTO> current_state;
    Statement fact(database_,
                   "SELECT component_lifecycle_generation,"
                   "component_lifecycle_state FROM "
                   "lifecycle_component_current_facts WHERE "
                   "controllable_component_id=?");
    fact.Bind(1, controllable_component_id.value());
    if (fact.Step()) {
      const auto fact_generation =
          Positive(fact.Integer(0), "current component lifecycle generation");
      const auto raw_state = fact.Integer(1);
      const auto state = static_cast<ComponentLifecycleStateDTO>(raw_state);
      Require(durable_generation.has_value() && durable_generation->value() == fact_generation &&
                  raw_state >= 0 && raw_state <= 7 && !to_string(state).empty() && !fact.Step(),
              "component_lifecycle_journal_corrupt",
              "current component lifecycle fact is stale or invalid");
      current_state = state;
    }
    result.components.push_back({controllable_component_id, durable_generation, current_state});
  }
  result.domains.reserve(canonical_control_domain_ids.size());
  for (const auto& control_domain_id : canonical_control_domain_ids) {
    std::optional<ActiveControlConfigurationDTO> configuration;
    Statement retained(database_,
                       "SELECT active_control_configuration_generation,configuration_blob "
                       "FROM control_domain_active_configurations WHERE control_domain_id=?");
    retained.Bind(1, control_domain_id.value());
    if (retained.Step()) {
      const auto stored_generation =
          Positive(retained.Integer(0), "current active-configuration generation");
      configuration = detail::DecodeActiveControlConfiguration(retained.Blob(1));
      Require(
          !retained.Step() && configuration->control_domain_id == control_domain_id &&
              configuration->active_control_configuration_generation.value() == stored_generation,
          "component_lifecycle_journal_corrupt",
          "current active-control-configuration row is invalid or "
          "duplicated");
    }
    result.domains.push_back({control_domain_id, std::move(configuration)});
  }
  Require(result.locally_valid(), "component_lifecycle_journal_corrupt",
          "current component snapshot fact readback is invalid");
  transaction.Commit();
  return result;
}

void SqliteCCLCJournal::Impl::RequireRouteEffectProjection(const RouteSelectionStatusDTO& status) {
  std::vector<ControllerLifecycleTransitionResultDTO> completed_controller_results;
  std::optional<RouteConfigurationFenceAcceptedDTO> completed_fence;
  std::optional<RouteConfigurationFenceReleasedDTO> completed_release;
  Statement completed_route_effects(
      database_,
      "SELECT completed_attempt_blob FROM cclc_route_effect_attempts WHERE "
      "route_selection_transaction_id=? AND completed_attempt_blob IS NOT NULL "
      "ORDER BY effect_sequence");
  completed_route_effects.Bind(1, status.acceptance.route_selection_transaction_id.value());
  while (completed_route_effects.Step()) {
    const auto completed = detail::DecodeCCLCRouteEffectAttempt(completed_route_effects.Blob(0));
    switch (completed.effect_kind) {
      case CCLCRouteEffectKindDTO::kKernelRouteConfigurationFenceAcquire: {
        const auto& result =
            std::get<RouteConfigurationFenceAcquireEffectResultDTO>(*completed.result);
        if (result.disposition == RouteConfigurationFenceAcquireEffectDispositionDTO::kAccepted) {
          Require(!completed_fence.has_value() && result.acceptance.has_value(),
                  "component_lifecycle_journal_attempt_conflict",
                  "route status has duplicate accepted-fence evidence");
          completed_fence = result.acceptance;
        }
        break;
      }
      case CCLCRouteEffectKindDTO::kControllerLifecycleTransition:
        completed_controller_results.push_back(
            std::get<ControllerLifecycleTransitionResultDTO>(*completed.result));
        break;
      case CCLCRouteEffectKindDTO::kKernelRouteConfigurationFenceRelease: {
        const auto& result =
            std::get<RouteConfigurationFenceReleaseEffectResultDTO>(*completed.result);
        if (result.disposition == RouteConfigurationFenceReleaseEffectDispositionDTO::kReleased) {
          Require(!completed_release.has_value() && result.receipt.has_value(),
                  "component_lifecycle_journal_attempt_conflict",
                  "route status has duplicate released-fence evidence");
          completed_release = result.receipt;
        }
        break;
      }
    }
  }
  Require(completed_controller_results == status.controller_transition_results &&
              completed_fence == status.accepted_fence &&
              completed_release == status.released_fence,
          "component_lifecycle_journal_attempt_conflict",
          "route status conflicts with completed effect evidence");
}

void SqliteCCLCJournal::Impl::RetainCurrentActiveControlConfiguration(
    const RouteSelectionStatusDTO& status) {
  if (!status.resulting_active_control_configuration.has_value()) {
    return;
  }

  const auto& configuration = *status.resulting_active_control_configuration;
  const auto configuration_blob = detail::EncodeActiveControlConfiguration(configuration);
  const auto generation = configuration.active_control_configuration_generation.value();

  Statement watermark(database_,
                      "SELECT active_control_configuration_generation FROM "
                      "control_domain_active_configuration_watermarks WHERE control_domain_id=?");
  watermark.Bind(1, configuration.control_domain_id.value());
  Require(watermark.Step() &&
              Positive(watermark.Integer(0), "active-configuration generation watermark") >=
                  generation &&
              !watermark.Step(),
          "component_lifecycle_journal_generation_conflict",
          "published active configuration is not covered by its durable watermark");

  Statement origin(database_,
                   "SELECT route_selection_transaction_generation FROM "
                   "cclc_route_transaction_identity_guards WHERE route_selection_transaction_id=?");
  origin.Bind(1, status.acceptance.route_selection_transaction_id.value());
  Require(origin.Step() &&
              Positive(origin.Integer(0), "route-selection transaction generation") ==
                  status.acceptance.route_selection_transaction_generation.value() &&
              !origin.Step(),
          "component_lifecycle_journal_corrupt",
          "published active configuration has no exact route transaction origin");

  Statement retained(database_,
                     "SELECT active_control_configuration_generation,"
                     "origin_route_selection_transaction_id,"
                     "origin_route_selection_transaction_generation,configuration_blob "
                     "FROM control_domain_active_configurations WHERE control_domain_id=?");
  retained.Bind(1, configuration.control_domain_id.value());
  if (retained.Step()) {
    const auto retained_generation =
        Positive(retained.Integer(0), "current active-configuration generation");
    if (retained_generation == generation) {
      Require(!retained.IsNull(1) && !retained.IsNull(2) &&
                  retained.Text(1) == status.acceptance.route_selection_transaction_id.value() &&
                  Positive(retained.Integer(2),
                           "current active-configuration origin transaction generation") ==
                      status.acceptance.route_selection_transaction_generation.value() &&
                  retained.Blob(3) == configuration_blob && !retained.Step(),
              "component_lifecycle_journal_generation_conflict",
              "active-configuration generation was already published differently");
      return;
    }
    Require(retained_generation < generation && !retained.Step(),
            "component_lifecycle_journal_generation_conflict",
            "active-configuration publication would regress current truth");

    Statement update(database_,
                     "UPDATE control_domain_active_configurations SET "
                     "active_control_configuration_generation=?,"
                     "origin_route_selection_transaction_id=?,"
                     "origin_route_selection_transaction_generation=?,configuration_blob=? "
                     "WHERE control_domain_id=?");
    update.Bind(1, static_cast<std::int64_t>(generation));
    update.Bind(2, status.acceptance.route_selection_transaction_id.value());
    update.Bind(3, static_cast<std::int64_t>(
                       status.acceptance.route_selection_transaction_generation.value()));
    update.BindBlob(4, configuration_blob);
    update.Bind(5, configuration.control_domain_id.value());
    Require(!update.Step() && sqlite3_changes(database_) == 1,
            "component_lifecycle_journal_corrupt",
            "active-configuration publication did not replace exactly one row");
    return;
  }

  Statement insert(
      database_,
      "INSERT INTO control_domain_active_configurations("
      "control_domain_id,active_control_configuration_generation,"
      "origin_route_selection_transaction_id,origin_route_selection_transaction_generation,"
      "configuration_blob) VALUES(?,?,?,?,?)");
  insert.Bind(1, configuration.control_domain_id.value());
  insert.Bind(2, static_cast<std::int64_t>(generation));
  insert.Bind(3, status.acceptance.route_selection_transaction_id.value());
  insert.Bind(4, static_cast<std::int64_t>(
                     status.acceptance.route_selection_transaction_generation.value()));
  insert.BindBlob(5, configuration_blob);
  Require(!insert.Step(), "component_lifecycle_journal_corrupt",
          "active-configuration publication insert returned an unexpected row");
}

std::uint64_t SqliteCCLCJournal::Impl::RecordRouteEffectPrepared(
    const RequestOwnerEpoch& active_owner_epoch, const CCLCRouteEffectAttemptDTO& prepared_attempt,
    const RouteSelectionStatusDTO& successor_status) {
  Require(!active_owner_epoch.empty() && prepared_attempt.locally_valid() &&
              prepared_attempt.state == CCLCRouteEffectAttemptStateDTO::kPrepared &&
              successor_status.locally_valid() &&
              successor_status.status_owner_epoch == active_owner_epoch &&
              FitsSqliteInteger(prepared_attempt.effect_sequence) &&
              FitsSqliteInteger(successor_status.status_sequence),
          "component_lifecycle_journal_attempt_invalid",
          "route prepared effect or successor status is invalid");
  const auto attempt_blob = detail::EncodeCCLCRouteEffectAttempt(prepared_attempt);
  const auto status_blob = detail::EncodeRouteSelectionStatus(successor_status);
  const auto status_digest = content_identity::sha256_digest(status_blob);
  const auto effect_identity = RouteEffectIdentity(prepared_attempt);

  std::lock_guard lock(mutex_);
  RequireOpen();
  Transaction transaction(database_);
  Require(ActiveOwner(active_owner_epoch), "component_lifecycle_journal_owner_epoch_stale",
          "route prepared-effect writer does not own the active epoch");

  Statement retained(database_,
                     "SELECT terminal,status_blob FROM route_selection_transactions "
                     "WHERE route_selection_transaction_id=?");
  retained.Bind(1, prepared_attempt.route_selection_transaction_id.value());
  Require(retained.Step(), "component_lifecycle_journal_transaction_missing",
          "route prepared-effect transaction is not retained");
  const auto terminal = retained.Integer(0);
  const auto previous = detail::DecodeRouteSelectionStatus(retained.Blob(1));
  Require(!retained.Step(), "component_lifecycle_journal_corrupt",
          "route prepared-effect transaction is duplicated");

  Statement existing(database_,
                     "SELECT attempt_sequence,route_selection_transaction_id,effect_sequence,"
                     "effect_kind,prepared_status_digest,prepared_attempt_blob,"
                     "completed_attempt_blob FROM cclc_route_effect_attempts "
                     "WHERE effect_identity=?");
  existing.Bind(1, effect_identity);
  if (existing.Step()) {
    const auto row_sequence = Positive(existing.Integer(0), "route effect-attempt row sequence");
    const bool exact =
        existing.Text(1) == prepared_attempt.route_selection_transaction_id.value() &&
        Positive(existing.Integer(2), "route effect sequence") ==
            prepared_attempt.effect_sequence &&
        existing.Text(3) == RouteEffectKindName(prepared_attempt.effect_kind) &&
        existing.Text(4) == status_digest && existing.Blob(5) == attempt_blob && existing.IsNull(6);
    Require(!existing.Step() && exact, "component_lifecycle_journal_attempt_conflict",
            "route effect identity was already prepared differently");
    transaction.Commit();
    return row_sequence;
  }

  Statement unresolved(database_,
                       "SELECT count(*) FROM cclc_route_effect_attempts WHERE "
                       "route_selection_transaction_id=? AND completed_attempt_blob IS NULL");
  unresolved.Bind(1, prepared_attempt.route_selection_transaction_id.value());
  Require(unresolved.Step() && unresolved.Integer(0) == 0 && !unresolved.Step(),
          "component_lifecycle_journal_attempt_unresolved",
          "route transaction already has an unresolved external effect");

  Require(terminal == 0 && previous.status_owner_epoch == active_owner_epoch &&
              successor_status.acceptance.route_selection_transaction_id ==
                  prepared_attempt.route_selection_transaction_id &&
              successor_status.acceptance.route_selection_transaction_generation ==
                  prepared_attempt.route_selection_transaction_generation &&
              RouteStatusReplacementIsMonotonic(previous, successor_status, false) &&
              !Terminal(successor_status),
          "component_lifecycle_journal_status_regressed",
          "route prepared-effect successor status is not monotonic and nonterminal");

  switch (prepared_attempt.effect_kind) {
    case CCLCRouteEffectKindDTO::kKernelRouteConfigurationFenceAcquire: {
      const auto& request = std::get<RouteConfigurationFenceIntentDTO>(prepared_attempt.request);
      Require(request.route_selection_transaction_id ==
                      previous.acceptance.route_selection_transaction_id &&
                  request.route_selection_transaction_generation ==
                      previous.acceptance.route_selection_transaction_generation &&
                  request.candidate_active_control_configuration_id ==
                      previous.acceptance.candidate_active_control_configuration_id &&
                  request.candidate_active_control_configuration_generation ==
                      previous.acceptance.candidate_active_control_configuration_generation &&
                  successor_status.phase == RouteSelectionTransactionPhaseDTO::kAcquiringFence &&
                  !successor_status.accepted_fence.has_value(),
              "component_lifecycle_journal_attempt_invalid",
              "route-fence Acquire does not match the accepted route");
      break;
    }
    case CCLCRouteEffectKindDTO::kControllerLifecycleTransition: {
      const auto& request =
          std::get<ControllerLifecycleTransitionIntentDTO>(prepared_attempt.request);
      Require(previous.accepted_fence.has_value() &&
                  previous.controller_transition_plan.has_value() &&
                  request.step_index < previous.controller_transition_plan->steps.size() &&
                  request.step == previous.controller_transition_plan->steps[request.step_index] &&
                  request.query == previous.acceptance.controller_lifecycle_observation_query &&
                  request.expected_source_sequence ==
                      previous.controller_transition_plan->expected_source_sequence &&
                  successor_status.controller_transition_results ==
                      previous.controller_transition_results &&
                  (successor_status.phase == RouteSelectionTransactionPhaseDTO::kDeactivating ||
                   successor_status.phase == RouteSelectionTransactionPhaseDTO::kActivating),
              "component_lifecycle_journal_attempt_invalid",
              "controller transition is outside the retained route plan");
      break;
    }
    case CCLCRouteEffectKindDTO::kKernelRouteConfigurationFenceRelease: {
      const auto& request =
          std::get<RouteConfigurationFenceReleaseRequestDTO>(prepared_attempt.request);
      Require(previous.accepted_fence.has_value() &&
                  request.accepted_fence == *previous.accepted_fence &&
                  request.release_precondition_status_sequence == previous.status_sequence &&
                  request.release_precondition_status_digest.value() ==
                      detail::RouteSelectionStatusReleasePreconditionDigest(previous) &&
                  successor_status.phase == RouteSelectionTransactionPhaseDTO::kReleasingFence &&
                  successor_status.accepted_fence == previous.accepted_fence &&
                  !successor_status.released_fence.has_value(),
              "component_lifecycle_journal_attempt_invalid",
              "route-fence Release was not prepared from the exact durable status");
      break;
    }
  }

  const auto highest_effect = Nonnegative(
      [&]() {
        Statement highest(database_,
                          "SELECT coalesce(max(effect_sequence),0) FROM cclc_route_effect_attempts "
                          "WHERE route_selection_transaction_id=?");
        highest.Bind(1, prepared_attempt.route_selection_transaction_id.value());
        Require(highest.Step(), "component_lifecycle_journal_corrupt",
                "highest route effect sequence is missing");
        const auto value = highest.Integer(0);
        Require(!highest.Step(), "component_lifecycle_journal_corrupt",
                "highest route effect sequence is duplicated");
        return value;
      }(),
      "highest route effect sequence");
  Require(highest_effect < static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) &&
              prepared_attempt.effect_sequence == highest_effect + 1U,
          "component_lifecycle_journal_attempt_sequence_conflict",
          "route effect sequence is not the next durable value");

  const auto retained_bytes = Nonnegative(
      [&]() {
        Statement bytes(database_,
                        "SELECT coalesce(sum(length(prepared_attempt_blob)+"
                        "coalesce(length(completed_attempt_blob),0)),0) FROM "
                        "cclc_route_effect_attempts WHERE route_selection_transaction_id=?");
        bytes.Bind(1, prepared_attempt.route_selection_transaction_id.value());
        Require(bytes.Step(), "component_lifecycle_journal_corrupt",
                "route effect-history byte count is missing");
        const auto value = bytes.Integer(0);
        Require(!bytes.Step(), "component_lifecycle_journal_corrupt",
                "route effect-history byte count is duplicated");
        return value;
      }(),
      "route effect-history bytes");
  Require(
      retained_bytes <= detail::kMaximumComponentLifecycleJournalRecordBytes - attempt_blob.size(),
      "component_lifecycle_journal_capacity_exhausted",
      "route transaction effect history would exceed its byte bound");

  Statement insert(database_,
                   "INSERT INTO cclc_route_effect_attempts("
                   "route_selection_transaction_id,effect_sequence,effect_kind,"
                   "effect_identity,prepared_owner_epoch,prepared_status_sequence,"
                   "prepared_status_digest,prepared_attempt_blob) VALUES(?,?,?,?,?,?,?,?)");
  insert.Bind(1, prepared_attempt.route_selection_transaction_id.value());
  insert.Bind(2, static_cast<std::int64_t>(prepared_attempt.effect_sequence));
  insert.Bind(3, RouteEffectKindName(prepared_attempt.effect_kind));
  insert.Bind(4, effect_identity);
  insert.Bind(5, active_owner_epoch.value());
  insert.Bind(6, static_cast<std::int64_t>(successor_status.status_sequence));
  insert.Bind(7, status_digest);
  insert.BindBlob(8, attempt_blob);
  Require(!insert.Step(), "component_lifecycle_journal_corrupt",
          "route prepared-effect insert returned an unexpected row");
  const auto row_sequence = sqlite3_last_insert_rowid(database_);
  Require(row_sequence > 0, "component_lifecycle_journal_corrupt",
          "route effect-attempt sequence is invalid");

  RetainCurrentActiveControlConfiguration(successor_status);
  Statement update_status(database_,
                          "UPDATE route_selection_transactions SET status_sequence=?,"
                          "latest_updated_at_ns=?,status_blob=? "
                          "WHERE route_selection_transaction_id=?");
  update_status.Bind(1, static_cast<std::int64_t>(successor_status.status_sequence));
  update_status.Bind(2, successor_status.latest_updated_at_ns);
  update_status.BindBlob(3, status_blob);
  update_status.Bind(4, prepared_attempt.route_selection_transaction_id.value());
  Require(!update_status.Step() && sqlite3_changes(database_) == 1,
          "component_lifecycle_journal_corrupt",
          "route prepared-effect status did not replace one transaction");
  transaction.Commit();
  return static_cast<std::uint64_t>(row_sequence);
}

std::uint64_t SqliteCCLCJournal::Impl::RecordRouteEffectCompleted(
    const RequestOwnerEpoch& active_owner_epoch, const CCLCRouteEffectAttemptDTO& completed_attempt,
    const RouteSelectionStatusDTO& successor_status, const CellAdmissionClaimReleaseDTO* release) {
  Require(!Terminal(successor_status) ||
              !successor_status.accepted_intent.expected_admission_generation || release,
          "component_lifecycle_claim_release_invalid",
          "new terminal route requires atomic claim release");
  Require(!active_owner_epoch.empty() && completed_attempt.locally_valid() &&
              completed_attempt.state == CCLCRouteEffectAttemptStateDTO::kCompleted &&
              successor_status.locally_valid() &&
              successor_status.status_owner_epoch == active_owner_epoch &&
              FitsSqliteInteger(completed_attempt.effect_sequence) &&
              FitsSqliteInteger(successor_status.status_sequence),
          "component_lifecycle_journal_attempt_invalid",
          "route completed effect or successor status is invalid");
  const auto prepared_form = PreparedForm(completed_attempt);
  const auto prepared_blob = detail::EncodeCCLCRouteEffectAttempt(prepared_form);
  const auto completed_blob = detail::EncodeCCLCRouteEffectAttempt(completed_attempt);
  const auto status_blob = detail::EncodeRouteSelectionStatus(successor_status);
  const auto status_digest = content_identity::sha256_digest(status_blob);
  const auto effect_identity = RouteEffectIdentity(completed_attempt);

  std::lock_guard lock(mutex_);
  RequireOpen();
  Transaction transaction(database_);
  Require(ActiveOwner(active_owner_epoch), "component_lifecycle_journal_owner_epoch_stale",
          "route completed-effect writer does not own the active epoch");

  Statement attempt(database_,
                    "SELECT attempt_sequence,prepared_attempt_blob,"
                    "completed_attempt_blob,completed_status_digest "
                    "FROM cclc_route_effect_attempts "
                    "WHERE effect_identity=?");
  attempt.Bind(1, effect_identity);
  Require(attempt.Step(), "component_lifecycle_journal_attempt_missing",
          "route completed effect has no prepared row");
  const auto row_sequence = Positive(attempt.Integer(0), "route effect-attempt row sequence");
  const auto retained_prepared_blob = attempt.Blob(1);
  if (!attempt.IsNull(2)) {
    const bool exact = retained_prepared_blob == prepared_blob &&
                       attempt.Blob(2) == completed_blob && !attempt.IsNull(3) &&
                       attempt.Text(3) == status_digest;
    Require(!attempt.Step() && exact, "component_lifecycle_journal_attempt_conflict",
            "route effect was already completed differently");
    transaction.Commit();
    return row_sequence;
  }
  Require(!attempt.Step() && attempt.IsNull(3) && retained_prepared_blob == prepared_blob,
          "component_lifecycle_journal_attempt_conflict",
          "route completion conflicts with the prepared effect");

  Statement retained(database_,
                     "SELECT terminal,status_blob FROM route_selection_transactions "
                     "WHERE route_selection_transaction_id=?");
  retained.Bind(1, completed_attempt.route_selection_transaction_id.value());
  Require(retained.Step(), "component_lifecycle_journal_transaction_missing",
          "route completed-effect transaction is not retained");
  const auto terminal = retained.Integer(0);
  const auto previous = detail::DecodeRouteSelectionStatus(retained.Blob(1));
  Require(!retained.Step(), "component_lifecycle_journal_corrupt",
          "route completed-effect transaction is duplicated");
  Require(terminal == 0 && RouteStatusReplacementIsMonotonic(previous, successor_status, true),
          "component_lifecycle_journal_status_regressed",
          "route completed-effect successor status is not monotonic");

  switch (completed_attempt.effect_kind) {
    case CCLCRouteEffectKindDTO::kKernelRouteConfigurationFenceAcquire: {
      const auto& result =
          std::get<RouteConfigurationFenceAcquireEffectResultDTO>(*completed_attempt.result);
      if (result.disposition == RouteConfigurationFenceAcquireEffectDispositionDTO::kAccepted) {
        Require(successor_status.accepted_fence == result.acceptance && !Terminal(successor_status),
                "component_lifecycle_journal_attempt_conflict",
                "accepted route fence is absent from the successor status");
      } else {
        Require(!successor_status.accepted_fence.has_value() &&
                    successor_status.terminal_result ==
                        std::optional(RouteSelectionTerminalResultDTO::kFailedKnownNoEffect),
                "component_lifecycle_journal_attempt_conflict",
                "proven-not-seen route fence manufactured fence evidence");
      }
      break;
    }
    case CCLCRouteEffectKindDTO::kControllerLifecycleTransition: {
      const auto& result =
          std::get<ControllerLifecycleTransitionResultDTO>(*completed_attempt.result);
      Require(successor_status.controller_transition_results.size() ==
                      previous.controller_transition_results.size() + 1U &&
                  std::equal(previous.controller_transition_results.begin(),
                             previous.controller_transition_results.end(),
                             successor_status.controller_transition_results.begin()) &&
                  successor_status.controller_transition_results.back() == result &&
                  !Terminal(successor_status),
              "component_lifecycle_journal_attempt_conflict",
              "controller transition result is not append-only in route status");
      break;
    }
    case CCLCRouteEffectKindDTO::kKernelRouteConfigurationFenceRelease: {
      const auto& result =
          std::get<RouteConfigurationFenceReleaseEffectResultDTO>(*completed_attempt.result);
      if (result.disposition == RouteConfigurationFenceReleaseEffectDispositionDTO::kReleased) {
        Require(successor_status.released_fence == result.receipt && Terminal(successor_status),
                "component_lifecycle_journal_attempt_conflict",
                "released route fence is absent from terminal successor status");
      } else {
        Require(!successor_status.released_fence.has_value() && !Terminal(successor_status),
                "component_lifecycle_journal_attempt_conflict",
                "proven-held route fence completion manufactured a release");
      }
      break;
    }
  }

  const auto retained_bytes = Nonnegative(
      [&]() {
        Statement bytes(database_,
                        "SELECT coalesce(sum(length(prepared_attempt_blob)+"
                        "coalesce(length(completed_attempt_blob),0)),0) FROM "
                        "cclc_route_effect_attempts WHERE route_selection_transaction_id=?");
        bytes.Bind(1, completed_attempt.route_selection_transaction_id.value());
        Require(bytes.Step(), "component_lifecycle_journal_corrupt",
                "route effect-history byte count is missing");
        const auto value = bytes.Integer(0);
        Require(!bytes.Step(), "component_lifecycle_journal_corrupt",
                "route effect-history byte count is duplicated");
        return value;
      }(),
      "route effect-history bytes");
  Require(retained_bytes <=
              detail::kMaximumComponentLifecycleJournalRecordBytes - completed_blob.size(),
          "component_lifecycle_journal_capacity_exhausted",
          "route transaction effect history would exceed its byte bound");
  if (Terminal(successor_status) &&
      !successor_status.accepted_intent.expected_admission_generation) {
    CompactTerminalResults(1U, static_cast<std::uint64_t>(status_blob.size()) + retained_bytes +
                                   completed_blob.size());
  }

  Statement update_attempt(database_,
                           "UPDATE cclc_route_effect_attempts SET completed_owner_epoch=?,"
                           "completed_status_sequence=?,completed_status_digest=?,"
                           "completed_attempt_blob=? WHERE attempt_sequence=?");
  update_attempt.Bind(1, active_owner_epoch.value());
  update_attempt.Bind(2, static_cast<std::int64_t>(successor_status.status_sequence));
  update_attempt.Bind(3, status_digest);
  update_attempt.BindBlob(4, completed_blob);
  update_attempt.Bind(5, static_cast<std::int64_t>(row_sequence));
  Require(!update_attempt.Step() && sqlite3_changes(database_) == 1,
          "component_lifecycle_journal_corrupt",
          "completed route effect did not update one prepared row");

  RequireRouteEffectProjection(successor_status);
  RetainCurrentActiveControlConfiguration(successor_status);
  if (Terminal(successor_status)) {
    Statement unresolved(database_,
                         "SELECT count(*) FROM cclc_route_effect_attempts WHERE "
                         "route_selection_transaction_id=? AND "
                         "completed_attempt_blob IS NULL");
    unresolved.Bind(1, completed_attempt.route_selection_transaction_id.value());
    Require(unresolved.Step() && unresolved.Integer(0) == 0 && !unresolved.Step(),
            "component_lifecycle_journal_attempt_unresolved",
            "terminal route status has an unresolved effect attempt");
  }

  const auto terminal_sequence = Terminal(successor_status)
                                     ? std::optional<std::uint64_t>(MintTerminalSequence())
                                     : std::nullopt;
  Statement update_status(database_,
                          "UPDATE route_selection_transactions SET status_sequence=?,"
                          "latest_updated_at_ns=?,terminal=?,terminal_sequence=?,status_blob=? "
                          "WHERE route_selection_transaction_id=?");
  update_status.Bind(1, static_cast<std::int64_t>(successor_status.status_sequence));
  update_status.Bind(2, successor_status.latest_updated_at_ns);
  update_status.Bind(3, Terminal(successor_status) ? 1 : 0);
  if (terminal_sequence.has_value()) {
    update_status.Bind(4, static_cast<std::int64_t>(*terminal_sequence));
  } else {
    update_status.BindNull(4);
  }
  update_status.BindBlob(5, status_blob);
  update_status.Bind(6, completed_attempt.route_selection_transaction_id.value());
  Require(!update_status.Step() && sqlite3_changes(database_) == 1,
          "component_lifecycle_journal_corrupt",
          "completed route effect status did not replace one transaction");
  if (release) {
    Require(Terminal(successor_status), "component_lifecycle_claim_release_invalid",
            "release requires terminal status");
    RetainCellClaimRelease(*release,
                           successor_status.acceptance.route_selection_transaction_id.value(),
                           detail::RouteSelectionStatusReleasePreconditionDigest(successor_status));
  }
  CompactTerminalResults();
  transaction.Commit();
  return row_sequence;
}

std::uint64_t SqliteCCLCJournal::Impl::RecordRouteSelectionStatus(
    const RequestOwnerEpoch& active_owner_epoch, const RouteSelectionStatusDTO& status,
    const CellAdmissionClaimReleaseDTO* release) {
  Require(!Terminal(status) || !status.accepted_intent.expected_admission_generation || release,
          "component_lifecycle_claim_release_invalid",
          "new terminal route requires atomic claim release");
  Require(!active_owner_epoch.empty() && status.locally_valid() &&
              status.status_owner_epoch == active_owner_epoch &&
              FitsSqliteInteger(status.status_sequence),
          "component_lifecycle_journal_record_invalid",
          "route-selection status is invalid at journal ingress");
  const auto encoded = detail::EncodeRouteSelectionStatus(status);
  std::lock_guard lock(mutex_);
  RequireOpen();
  Transaction transaction(database_);
  Require(ActiveOwner(active_owner_epoch), "component_lifecycle_journal_owner_epoch_stale",
          "route-selection status writer does not own the active epoch");

  Statement retained(database_,
                     "SELECT status_blob FROM route_selection_transactions "
                     "WHERE route_selection_transaction_id=?");
  retained.Bind(1, status.acceptance.route_selection_transaction_id.value());
  Require(retained.Step(), "component_lifecycle_journal_transaction_missing",
          "route-selection transaction is not retained");
  const auto previous = detail::DecodeRouteSelectionStatus(retained.Blob(0));
  Require(!retained.Step(), "component_lifecycle_journal_corrupt",
          "route-selection transaction identity is duplicated");

  Statement unresolved(database_,
                       "SELECT count(*) FROM cclc_route_effect_attempts WHERE "
                       "route_selection_transaction_id=? AND completed_attempt_blob IS NULL");
  unresolved.Bind(1, status.acceptance.route_selection_transaction_id.value());
  Require(unresolved.Step(), "component_lifecycle_journal_corrupt",
          "route unresolved-effect count is missing");
  const bool has_unresolved_prepared_effect = unresolved.Integer(0) != 0;
  Require(!unresolved.Step(), "component_lifecycle_journal_corrupt",
          "route unresolved-effect count is duplicated");

  Require(RouteStatusReplacementIsMonotonic(previous, status, has_unresolved_prepared_effect),
          "component_lifecycle_journal_status_regressed",
          "route-selection status replacement is not monotonic");
  RequireRouteEffectProjection(status);
  RetainCurrentActiveControlConfiguration(status);
  if (Terminal(status)) {
    Require(!has_unresolved_prepared_effect, "component_lifecycle_journal_attempt_unresolved",
            "terminal route status has an unresolved effect attempt");
    const auto retained_effect_bytes = Nonnegative(
        [&]() {
          Statement bytes(database_,
                          "SELECT coalesce(sum(length(prepared_attempt_blob)+"
                          "coalesce(length(completed_attempt_blob),0)),0) FROM "
                          "cclc_route_effect_attempts WHERE route_selection_transaction_id=?");
          bytes.Bind(1, status.acceptance.route_selection_transaction_id.value());
          Require(bytes.Step(), "component_lifecycle_journal_corrupt",
                  "terminal route effect-history byte count is missing");
          const auto value = bytes.Integer(0);
          Require(!bytes.Step(), "component_lifecycle_journal_corrupt",
                  "terminal route effect-history byte count is duplicated");
          return value;
        }(),
        "terminal route effect-history bytes");
    if (!status.accepted_intent.expected_admission_generation)
      CompactTerminalResults(1U,
                             static_cast<std::uint64_t>(encoded.size()) + retained_effect_bytes);
  }

  const auto terminal_sequence =
      Terminal(status) ? std::optional<std::uint64_t>(MintTerminalSequence()) : std::nullopt;
  Statement update(database_,
                   "UPDATE route_selection_transactions SET status_sequence=?,"
                   "latest_updated_at_ns=?,terminal=?,terminal_sequence=?,status_blob=? "
                   "WHERE route_selection_transaction_id=?");
  update.Bind(1, static_cast<std::int64_t>(status.status_sequence));
  update.Bind(2, status.latest_updated_at_ns);
  update.Bind(3, Terminal(status) ? 1 : 0);
  if (terminal_sequence.has_value()) {
    update.Bind(4, static_cast<std::int64_t>(*terminal_sequence));
  } else {
    update.BindNull(4);
  }
  update.BindBlob(5, encoded);
  update.Bind(6, status.acceptance.route_selection_transaction_id.value());
  Require(!update.Step() && sqlite3_changes(database_) == 1, "component_lifecycle_journal_corrupt",
          "route-selection status update did not affect exactly one row");
  if (release) {
    Require(Terminal(status), "component_lifecycle_claim_release_invalid",
            "release requires terminal status");
    RetainCellClaimRelease(*release, status.acceptance.route_selection_transaction_id.value(),
                           detail::RouteSelectionStatusReleasePreconditionDigest(status));
  }
  CompactTerminalResults();
  transaction.Commit();
  return status.status_sequence;
}

std::optional<RouteSelectionStatusDTO> SqliteCCLCJournal::Impl::LoadRetainedRouteSelectionStatus(
    const RouteSelectionTransactionId& route_selection_transaction_id) {
  Require(!route_selection_transaction_id.empty(), "component_lifecycle_journal_query_invalid",
          "route-selection transaction query identity is empty");
  std::lock_guard lock(mutex_);
  RequireOpen();
  Statement retained(database_,
                     "SELECT status_blob FROM route_selection_transactions "
                     "WHERE route_selection_transaction_id=?");
  retained.Bind(1, route_selection_transaction_id.value());
  if (!retained.Step()) {
    return std::nullopt;
  }
  auto status = detail::DecodeRouteSelectionStatus(retained.Blob(0));
  Require(!retained.Step(), "component_lifecycle_journal_corrupt",
          "route-selection transaction identity is duplicated");
  return status;
}

std::optional<ActiveControlConfigurationDTO>
SqliteCCLCJournal::Impl::LoadCurrentActiveControlConfiguration(
    const ControlDomainId& control_domain_id) {
  Require(!control_domain_id.empty() &&
              control_domain_id.value().size() <= kMaxRouteSelectionIdentityBytes,
          "component_lifecycle_journal_query_invalid",
          "active-control-configuration query domain is invalid");
  std::lock_guard lock(mutex_);
  RequireOpen();
  Statement retained(database_,
                     "SELECT active_control_configuration_generation,configuration_blob "
                     "FROM control_domain_active_configurations WHERE control_domain_id=?");
  retained.Bind(1, control_domain_id.value());
  if (!retained.Step()) {
    return std::nullopt;
  }
  const auto stored_generation =
      Positive(retained.Integer(0), "current active-configuration generation");
  auto configuration = detail::DecodeActiveControlConfiguration(retained.Blob(1));
  Require(!retained.Step() && configuration.control_domain_id == control_domain_id &&
              configuration.active_control_configuration_generation.value() == stored_generation,
          "component_lifecycle_journal_corrupt",
          "current active-control-configuration row is invalid or duplicated");
  return configuration;
}

void SqliteCCLCJournal::Impl::RequireNativeApplyCapacityLocked(
    const std::vector<ControllableComponentId>& components,
    const std::vector<ControlDomainId>& domains) {
  // Removed and abandoned identities retain generation history. Bound the
  // complete durable namespace before reserving or adopting a new identity,
  // not merely the number of components present in this candidate.
  const auto check = [&](const auto& candidate, const std::string_view query,
                          const std::size_t capacity) {
    std::set<std::string> identities;
    Statement rows(database_, query);
    while (rows.Step()) identities.insert(rows.Text(0));
    for (const auto& id : candidate) identities.insert(id.value());
    Require(identities.size() <= capacity,
            "component_lifecycle_journal_capacity_exhausted",
            "native Apply exceeds the durable lifecycle or route identity capacity");
  };
  check(components,
        "SELECT controllable_component_id FROM lifecycle_component_watermarks "
        "UNION SELECT identity FROM cclc_native_apply_reservations "
        "WHERE reservation_kind='component'",
        kMaximumCCLCComponentWatermarks);
  check(domains,
        "SELECT control_domain_id FROM control_domain_active_configuration_watermarks "
        "UNION SELECT identity FROM cclc_native_apply_reservations "
        "WHERE reservation_kind='route'",
        kMaximumCCLCActiveConfigurationWatermarks);
}

std::pair<std::uint64_t, std::uint64_t>
SqliteCCLCJournal::Impl::ReadNativeApplyGenerationValuesLocked(
    std::string_view kind, std::string_view identity) {
  const bool component = kind == "component";
  Statement watermark(database_,
                      component
                          ? "SELECT component_lifecycle_generation FROM "
                            "lifecycle_component_watermarks WHERE controllable_component_id=?"
                          : "SELECT active_control_configuration_generation FROM "
                            "control_domain_active_configuration_watermarks "
                            "WHERE control_domain_id=?");
  watermark.Bind(1, identity);
  std::uint64_t floor = 0U;
  if (watermark.Step()) {
    floor = Positive(watermark.Integer(0), "native Apply generation watermark");
    Require(!watermark.Step(), "component_lifecycle_journal_corrupt",
            "native Apply generation watermark is duplicated");
  }
  std::uint64_t reservation = 0U;
  Statement reserved(database_,
                     "SELECT generation FROM cclc_native_apply_reservations "
                     "WHERE reservation_kind=? AND identity=?");
  reserved.Bind(1, kind);
  reserved.Bind(2, identity);
  if (reserved.Step()) {
    reservation = Positive(reserved.Integer(0), "native Apply reservation");
    Require(!reserved.Step(), "component_lifecycle_journal_corrupt",
            "native Apply reservation is duplicated");
  }
  return {floor, reservation};
}

std::uint64_t SqliteCCLCJournal::Impl::NativeApplyGenerationFloorLocked(
    std::string_view kind, std::string_view identity) {
  const auto [watermark, reservation] = ReadNativeApplyGenerationValuesLocked(kind, identity);
  return std::max(watermark, reservation);
}

ApplyComponentGenerationSelectionDTO SqliteCCLCJournal::Impl::SelectApplyComponentGenerationLocked(
    std::string_view identity, const std::optional<ApplyComponentQualifiedPredecessorDTO>& qualified) {
  Require(!qualified || qualified->locally_valid(),
          "component_lifecycle_journal_query_invalid", "delegated qualified predecessor is invalid");
  const auto [watermark, reservation] = ReadNativeApplyGenerationValuesLocked("component", identity);
  Require(!qualified || watermark == 0U || watermark == qualified->generation.value(),
          "component_lifecycle_journal_generation_conflict",
          "delegated qualified predecessor differs from the durable watermark");
  const auto floor = std::max({watermark, reservation, qualified ? qualified->generation.value() : 0U});
  Require(floor < static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()),
          "component_lifecycle_native_apply_generation_exhausted",
          "native Apply component generation cannot advance");
  return {watermark ? std::optional<ComponentLifecycleGeneration>(ComponentLifecycleGeneration(watermark))
                    : std::nullopt,
          ComponentLifecycleGeneration(floor + 1U), reservation, qualified};
}

ApplyComponentGenerationSelectionDTO SqliteCCLCJournal::Impl::ReadApplyComponentGenerationSelection(
    const RequestOwnerEpoch& owner, const ControllableComponentId& component,
    const std::optional<ApplyComponentQualifiedPredecessorDTO>& qualified) {
  Require(!owner.empty() && owner.value().size() <= kMaxLifecycleRequestLabelBytes &&
              !component.empty() && component.value().size() <= kMaxLifecycleIdentityBytes &&
              (!qualified || qualified->locally_valid()),
          "component_lifecycle_journal_query_invalid", "delegated generation query is invalid");
  std::lock_guard lock(mutex_);
  RequireOpen();
  ReadTransaction transaction(database_);
  Require(ActiveOwner(owner), "component_lifecycle_journal_owner_epoch_stale",
          "delegated generation query does not name the current owner");
  const auto selected = SelectApplyComponentGenerationLocked(component.value(), qualified);
  transaction.Commit();
  return selected;
}

void SqliteCCLCJournal::Impl::RefreshNativeApplyPreparationFailure() {
  if (!preparation_failure_source_) return;
  std::optional<NativeApplyPlanDTO> captured;
  {
    std::lock_guard lock(mutex_);
    RequireOpen();
    // Read the raw persisted identity even if an earlier read classified it.
    Statement pending(database_,
        "SELECT p.apply_id,p.apply_generation FROM cclc_native_apply_plans p "
        "WHERE NOT EXISTS(SELECT 1 FROM cclc_native_apply_adoptions a "
        "WHERE a.apply_id=p.apply_id AND a.apply_generation=p.apply_generation)");
    if (pending.Step()) {
      const NamedApplyPublicationTransactionDTO apply{
          ApplyId(pending.Text(0)),
          ApplyGeneration(Positive(pending.Integer(1), "native Apply generation"))};
      Require(apply.valid() && !pending.Step(), "component_lifecycle_journal_corrupt",
              "unsettled native Apply identity is invalid or duplicated");
      captured = ReadNativeApplyPlanLocked(apply);
    }
  }
  bool observed = false;
  if (captured) {
    try {
      const auto result = preparation_failure_source_(*captured);
      observed = result.ok() && result.value();
    } catch (...) {
      // Unavailable parent evidence explains nothing and keeps the gate.
    }
  }
  std::lock_guard lock(mutex_);
  RequireOpen();
  observed_preparation_failure_.reset();
  if (observed && captured) {
    const auto current = ReadNativeApplyPlanLocked(captured->apply);
    if (current && *current == *captured)
      observed_preparation_failure_ = std::move(captured);
  }
}

std::optional<NamedApplyPublicationTransactionDTO>
SqliteCCLCJournal::Impl::ReadUnsettledNativeApplyPlan() {
  RefreshNativeApplyPreparationFailure();
  std::lock_guard lock(mutex_);
  RequireOpen();
  return ReadUnsettledNativeApplyPlanLocked();
}

std::vector<ControllableComponentId>
SqliteCCLCJournal::Impl::LoadComponentsWithUnsettledLifecycle() {
  std::lock_guard lock(mutex_);
  RequireOpen();
  Statement pending(database_,
                    "SELECT DISTINCT controllable_component_id FROM lifecycle_transactions "
                    "WHERE terminal=0 ORDER BY controllable_component_id");
  std::vector<ControllableComponentId> result;
  while (pending.Step()) {
    result.emplace_back(pending.Text(0));
    Require(!result.back().empty() && result.size() <= kMaxLifecycleFacts,
            "component_lifecycle_journal_corrupt",
            "unsettled lifecycle component identity is invalid or unbounded");
  }
  return result;
}

std::optional<NamedApplyPublicationTransactionDTO>
SqliteCCLCJournal::Impl::ReadUnsettledNativeApplyPlanLocked() {
  Statement pending(database_,
                    "SELECT p.apply_id,p.apply_generation FROM cclc_native_apply_plans p "
                    "WHERE NOT EXISTS(SELECT 1 FROM cclc_native_apply_adoptions a "
                    "WHERE a.apply_id=p.apply_id AND a.apply_generation=p.apply_generation)");
  if (!pending.Step()) return std::nullopt;
  NamedApplyPublicationTransactionDTO result{
      ApplyId(pending.Text(0)),
      ApplyGeneration(Positive(pending.Integer(1), "unsettled native Apply generation"))};
  Require(result.valid() && !pending.Step(), "component_lifecycle_journal_corrupt",
          "unsettled native Apply identity is invalid or duplicated");
  if (observed_preparation_failure_) {
    const auto plan = ReadNativeApplyPlanLocked(result);
    if (plan && *plan == *observed_preparation_failure_) return std::nullopt;
  }
  return result;
}

std::map<ControllableComponentId, ComponentLifecycleStatusDTO>
SqliteCCLCJournal::Impl::ReadNativeApplyChildrenLocked(
    const NativeApplyComponentBringUpDTO& link, const NamedApplyPublicationTransactionDTO& apply,
    const RetainedBundleId& candidate_bundle, const GraphRevision& candidate_graph, bool unstarted,
    const ActiveBundlePublicationReceiptDTO* publication) {
  Require(link.parent.owner.apply_id == apply.apply_id &&
              link.parent.owner.apply_generation == apply.apply_generation,
          "component_lifecycle_native_apply_invalid", "native child receipts name another parent");
  std::map<ControllableComponentId, ComponentLifecycleStatusDTO> children;
  std::optional<ApplyComponentBringUpAuthorityDTO> authority;
  std::optional<ProjectionRevisionDTO> projection;
  for (const auto& child : link.children) {
    Statement row(database_,
        "SELECT t.status_blob,t.component_lifecycle_generation,m.owner_epoch,m.request_id,"
        "m.semantic_request_digest,t.terminal FROM lifecycle_transactions t "
        "JOIN cclc_request_memberships m ON m.membership_sequence=t.membership_sequence "
        "WHERE t.lifecycle_transaction_id=? AND m.retained_result=1");
    row.Bind(1, child.acceptance.lifecycle_transaction_id.value());
    Require(row.Step(), "component_lifecycle_native_apply_unplanned",
            "native Apply has no exact retained delegated acceptance");
    auto status = detail::DecodeComponentLifecycleStatus(row.Blob(0));
    const auto terminal = row.Integer(5);
    Require(Positive(row.Integer(1), "native child generation") ==
                    child.acceptance.component_lifecycle_generation.value() &&
                row.Text(2) == child.request_context.request_owner_epoch.value() &&
                row.Text(3) == child.request_context.request_id.value() &&
                row.Text(4) == child.original_request_payload_digest.value() && !row.Step() &&
                status.locally_valid() && status.apply_bring_up && !status.selected_bring_up &&
                status.accepted_intent.apply_generation_selection &&
                status.accepted_intent.request_context == child.request_context &&
                status.accepted_intent.controllable_component_id == child.component_id &&
                status.acceptance == child.acceptance &&
                status.apply_bring_up->original_request_payload_digest ==
                    child.original_request_payload_digest &&
                status.apply_bring_up->parent_authority.parent == link.parent &&
                status.apply_bring_up->candidate_projection_revision.retained_bundle_id == candidate_bundle &&
                status.apply_bring_up->candidate_projection_revision.graph_revision == candidate_graph &&
                status.status_owner_epoch == child.request_context.request_owner_epoch &&
                ActiveOwner(child.request_context.request_owner_epoch) &&
                !status.reconciliation_required && !status.error && status.requested_state.state &&
                ReadNativeApplyGenerationValuesLocked("component", child.component_id.value()).first ==
                    child.acceptance.component_lifecycle_generation.value(),
            "component_lifecycle_native_apply_unplanned",
            "native Apply child differs from its exact original owner, parent, candidate or acceptance");
    const auto& actual_authority = status.apply_bring_up->parent_authority;
    const auto& actual_projection = status.apply_bring_up->candidate_projection_revision;
    Require((!authority || *authority == actual_authority) &&
                (!projection || *projection == actual_projection) &&
                (!publication || publication->request.candidate_projection_revision == actual_projection),
            "component_lifecycle_native_apply_unplanned",
            "native Apply children do not share the same full original claim, fence and publication candidate");
    authority = actual_authority;
    projection = actual_projection;
    Statement effects(database_,
        unstarted ? "SELECT 1 FROM cclc_effect_attempts WHERE lifecycle_transaction_id=?"
                  : "SELECT 1 FROM cclc_effect_attempts WHERE lifecycle_transaction_id=? "
                    "AND completed_attempt_blob IS NULL");
    effects.Bind(1, child.acceptance.lifecycle_transaction_id.value());
    Require(!effects.Step(), "component_lifecycle_native_apply_unsettled",
            "native Apply child has dispatched or unresolved effects");
    if (unstarted) {
      Require(terminal == 0 && status.phase == ComponentLifecycleTransactionPhaseDTO::kAccepted &&
                  status.status_sequence == 1U && !status.terminal_result &&
                  std::all_of(status.domains.begin(), status.domains.end(), [](const auto& domain) {
                    return domain.primitive_attempts.empty() && !domain.reconciliation_evidence && !domain.error &&
                        (domain.effect == ComponentLifecycleDomainEffectDTO::kNone ||
                         domain.effect == ComponentLifecycleDomainEffectDTO::kKnownNoEffect) &&
                        (domain.action_state == ComponentLifecycleDomainActionStateDTO::kNotStarted ||
                         domain.action_state == ComponentLifecycleDomainActionStateDTO::kReserved ||
                         domain.action_state == ComponentLifecycleDomainActionStateDTO::kSucceeded);
                  }),
              "component_lifecycle_native_apply_unsettled",
              "native Apply planning requires exact accepted unstarted children");
    } else {
      Require(terminal == 1 && status.phase == ComponentLifecycleTransactionPhaseDTO::kSucceeded &&
                  status.terminal_result == ComponentLifecycleTerminalResultDTO::kSucceeded &&
                  status.current_state == status.requested_state.state,
              "component_lifecycle_native_apply_unsettled",
              "native Apply adoption requires actual successful terminal children, not Holding or unknown work");
      for (const auto& domain : status.domains) {
        if (!domain.expected_domain) {
          Require(domain.action_state == ComponentLifecycleDomainActionStateDTO::kReserved &&
                      domain.effect == ComponentLifecycleDomainEffectDTO::kKnownNoEffect,
                  "component_lifecycle_native_apply_unsettled", "native Apply retired child domain is not settled");
          continue;
        }
        const auto& expected = *domain.expected_domain;
        Require(domain.current_scope && domain.resolved_provider_occurrence &&
                    domain.action_state == ComponentLifecycleDomainActionStateDTO::kSucceeded &&
                    domain.current_state == status.requested_state.state &&
                    domain.current_scope->control_domain_revision == expected.control_domain_revision &&
                    domain.current_scope->controller_topology_id == expected.controller_topology_id &&
                    domain.current_scope->topology_revision == expected.topology_revision &&
                    domain.current_scope->current_control_bindings == expected.expected_control_bindings,
                "component_lifecycle_native_apply_unsettled",
                "native Apply child lacks exact installed candidate scope, provider or requested postconditions");
      }
      Statement current(database_,
          "SELECT component_lifecycle_generation,component_lifecycle_state "
          "FROM lifecycle_component_current_facts WHERE controllable_component_id=?");
      current.Bind(1, child.component_id.value());
      Require(current.Step() && Positive(current.Integer(0), "native child current generation") ==
                  child.acceptance.component_lifecycle_generation.value() &&
                  static_cast<ComponentLifecycleStateDTO>(current.Integer(1)) == *status.current_state &&
                  !current.Step(),
              "component_lifecycle_native_apply_generation_stale",
              "native Apply child's actual settled lifecycle fact changed");
    }
    Require(children.emplace(child.component_id, std::move(status)).second,
            "component_lifecycle_native_apply_invalid", "native Apply child component is duplicated");
  }
  return children;
}

std::optional<NativeApplyPlanDTO> SqliteCCLCJournal::Impl::ReadNativeApplyPlanLocked(
    const NamedApplyPublicationTransactionDTO& apply) {
  Statement header(database_,
                   "SELECT apply_id,apply_generation,candidate_bundle_id,candidate_graph_revision,component_bring_up "
                   "FROM cclc_native_apply_plans WHERE singleton=1");
  if (!header.Step()) return std::nullopt;
  if (header.Text(0) != apply.apply_id.value() ||
      Positive(header.Integer(1), "native Apply plan generation") !=
          apply.apply_generation.value()) {
    return std::nullopt;
  }
  NativeApplyPlanDTO plan{apply, RetainedBundleId(header.Text(2)),
                          GraphRevision(header.Text(3)), {}, {}, {}, {}, {}, {}};
  if (!header.IsNull(4)) plan.component_bring_up = detail::DecodeNativeApplyComponentBringUp(header.Blob(4));
  Require(!header.Step(), "component_lifecycle_journal_corrupt",
          "native Apply plan header is duplicated");
  Statement components(database_,
                       "SELECT controllable_component_id,component_lifecycle_state,"
                       "component_lifecycle_generation,retained "
                       "FROM cclc_native_apply_plan_components "
                       "ORDER BY controllable_component_id");
  while (components.Step()) {
    const auto state = static_cast<ComponentLifecycleStateDTO>(components.Integer(1));
    const bool retained = components.Integer(3) == 1;
    // A changed component starts Active (native) or Declared (externally
    // managed controller), or keeps an explicit Disable; a retained one keeps
    // whatever exact state it already had.
    const bool delegated = plan.component_bring_up &&
        std::any_of(plan.component_bring_up->children.begin(), plan.component_bring_up->children.end(),
                    [&](const auto& child) { return child.component_id.value() == components.Text(0); });
    Require(retained ? !to_string(state).empty()
                     : ((delegated && state == ComponentLifecycleStateDTO::kPrepared) ||
                        state == ComponentLifecycleStateDTO::kActive ||
                        state == ComponentLifecycleStateDTO::kDeclared ||
                        state == ComponentLifecycleStateDTO::kDisabled),
            "component_lifecycle_journal_corrupt", "native Apply plan state is invalid");
    plan.components.push_back(
        {ControllableComponentId(components.Text(0)), state,
         ComponentLifecycleGeneration(
             Positive(components.Integer(2), "native Apply planned component generation"))});
    if (retained) plan.retained_components.push_back(plan.components.back().component_id);
  }
  Statement routes(database_,
                   "SELECT control_domain_id,active_control_configuration_generation,retained "
                   "FROM cclc_native_apply_plan_routes ORDER BY control_domain_id");
  while (routes.Step()) {
    plan.routes.push_back(
        {ControlDomainId(routes.Text(0)),
         ActiveControlConfigurationGeneration(
             Positive(routes.Integer(1), "native Apply planned route generation"))});
    if (routes.Integer(2) == 1) plan.retained_domains.push_back(plan.routes.back().control_domain_id);
  }
  Statement removals(database_,
                     "SELECT removal_kind,identity FROM cclc_native_apply_plan_removals "
                     "ORDER BY removal_kind,identity");
  while (removals.Step()) {
    if (removals.Text(0) == "component") {
      plan.removed_components.emplace_back(removals.Text(1));
    } else {
      plan.removed_domains.emplace_back(removals.Text(1));
    }
  }
  if (plan.component_bring_up) {
    Require(plan.component_bring_up->parent.owner.apply_id == apply.apply_id &&
                plan.component_bring_up->parent.owner.apply_generation == apply.apply_generation,
            "component_lifecycle_journal_corrupt", "native Apply link names another parent");
    for (const auto& child : plan.component_bring_up->children) {
      const auto found = std::find_if(plan.components.begin(), plan.components.end(),
          [&](const auto& component) { return component.component_id == child.component_id; });
      Require(found != plan.components.end() &&
                  found->generation == child.acceptance.component_lifecycle_generation &&
                  !std::binary_search(plan.retained_components.begin(), plan.retained_components.end(), child.component_id),
              "component_lifecycle_journal_corrupt", "native Apply link differs from its planned component rows");
    }
  }
  return plan;
}

NativeApplyPlanDTO SqliteCCLCJournal::Impl::PlanNativeApply(
    const NativeApplyPlanRequestDTO& request) {

  Require(request.apply.valid() && !request.candidate_retained_bundle_id.empty() &&
              !request.candidate_graph_revision.empty() &&
              request.components.size() <= kMaximumCCLCComponentWatermarks &&
              request.removed_components.size() <= kMaximumCCLCComponentWatermarks &&
              request.compiled_routes.size() <= kMaximumCCLCActiveConfigurationWatermarks &&
              request.removed_domains.size() <= kMaximumCCLCActiveConfigurationWatermarks &&
              (request.changed_component_state == ComponentLifecycleStateDTO::kActive ||
               request.changed_component_state == ComponentLifecycleStateDTO::kDeclared),
          "component_lifecycle_native_apply_invalid",
          "native Apply plan has an invalid Apply, candidate, scope or changed state");
  const auto valid_identity = [](const auto& id) {
    return !id.empty() && id.value().size() <= kMaxLifecycleIdentityBytes;
  };
  const auto sorted_unique = [](auto values) {
    std::sort(values.begin(), values.end());
    Require(std::adjacent_find(values.begin(), values.end()) == values.end(),
            "component_lifecycle_native_apply_invalid",
            "native Apply plan identities are duplicated");
    return values;
  };
  auto components = sorted_unique(request.components);
  const auto retained_components = sorted_unique(request.retained_components);
  const auto removed_components = sorted_unique(request.removed_components);
  const auto removed_domains = sorted_unique(request.removed_domains);
  Require(std::all_of(components.begin(), components.end(), valid_identity) &&
              std::all_of(removed_components.begin(), removed_components.end(),
                          valid_identity) &&
              std::all_of(removed_domains.begin(), removed_domains.end(), valid_identity) &&
              std::includes(components.begin(), components.end(),
                            retained_components.begin(), retained_components.end()) &&
              std::none_of(removed_components.begin(), removed_components.end(),
                           [&](const auto& id) {
                             return std::binary_search(components.begin(), components.end(), id);
                           }),
          "component_lifecycle_native_apply_invalid",
          "native Apply plan components are invalid, or retained/removed sets do not fit");
  auto compiled = request.compiled_routes;
  std::sort(compiled.begin(), compiled.end(), [](const auto& a, const auto& b) {
    return a.control_domain_id < b.control_domain_id;
  });
  for (std::size_t index = 0U; index < compiled.size(); ++index) {
    Require(!compiled[index].control_domain_id.empty() &&
                compiled[index].control_domain_id.value().size() <= kMaxLifecycleIdentityBytes &&
                compiled[index].generation.nonzero() &&
                FitsSqliteInteger(compiled[index].generation.value()) &&
                (index == 0U ||
                 compiled[index - 1U].control_domain_id != compiled[index].control_domain_id) &&
                !std::binary_search(removed_domains.begin(), removed_domains.end(),
                                    compiled[index].control_domain_id),
            "component_lifecycle_native_apply_invalid",
            "native Apply plan routes are invalid, duplicated or also removed");
  }
  std::map<ControlDomainId, ActiveControlConfigurationDTO> retained_routes;
  for (const auto& route : request.retained_routes) {
    Require(route.locally_valid() &&
                route.state == ActiveControlConfigurationStateDTO::kReconciled &&
                std::any_of(compiled.begin(), compiled.end(),
                            [&](const auto& row) {
                              return row.control_domain_id == route.control_domain_id;
                            }) &&
                retained_routes.emplace(route.control_domain_id, route).second,
            "component_lifecycle_native_apply_invalid",
            "native Apply retained route is invalid, duplicated or not planned");
  }
  std::vector<ControlDomainId> retained_domains;
  for (const auto& [domain, route] : retained_routes) retained_domains.push_back(domain);

  std::optional<std::string> link_blob;
  if (request.component_bring_up) {
    link_blob = detail::EncodeNativeApplyComponentBringUp(*request.component_bring_up);
    Require(request.component_bring_up->parent.owner.apply_id == request.apply.apply_id &&
                request.component_bring_up->parent.owner.apply_generation == request.apply.apply_generation &&
                std::all_of(request.component_bring_up->children.begin(), request.component_bring_up->children.end(),
                    [&](const auto& child) {
                      return std::binary_search(components.begin(), components.end(), child.component_id) &&
                          !std::binary_search(retained_components.begin(), retained_components.end(), child.component_id);
                    }),
            "component_lifecycle_native_apply_invalid", "native Apply linked children do not fit its changed scope");
  }
  std::lock_guard lock(mutex_);
  RequireOpen();
  Transaction transaction(database_);
  std::map<ControllableComponentId, ComponentLifecycleStatusDTO> children;
  bool replay = false;
  {
    Statement header(database_,
                     "SELECT apply_id,apply_generation,candidate_bundle_id,"
                     "candidate_graph_revision FROM cclc_native_apply_plans WHERE singleton=1");
    if (header.Step() && header.Text(0) == request.apply.apply_id.value() &&
        Positive(header.Integer(1), "native Apply plan generation") ==
            request.apply.apply_generation.value()) {
      // Exact replay of the same delegation returns the recorded plan; it
      // never recomputes states or generations.
      Require(header.Text(2) == request.candidate_retained_bundle_id.value() &&
                  header.Text(3) == request.candidate_graph_revision.value(),
              "component_lifecycle_native_apply_conflict",
              "native Apply was already planned for another candidate");
      replay = true;
    }
  }
  if (!replay) {
    if (request.component_bring_up) {
      children = ReadNativeApplyChildrenLocked(*request.component_bring_up, request.apply,
          request.candidate_retained_bundle_id, request.candidate_graph_revision, true);
      for (const auto& [component, child] : children) {
        for (const auto& expected : child.accepted_intent.expected_control_domains) {
          Require(std::any_of(compiled.begin(), compiled.end(), [&](const auto& route) {
                    return route.control_domain_id == expected.control_domain_id;
                  }),
                  "component_lifecycle_native_apply_invalid", "native Apply omits a delegated candidate domain");
        }
      }
    }
    Require(ActiveTransactionCount() == children.size() &&
                ScalarInteger(database_,
                              "SELECT count(*) FROM cclc_lifecycle_cell_claims "
                              "WHERE release_receipt_recorded=0") == 0,
            "component_lifecycle_native_apply_unsettled",
            "native Apply cannot be planned over unsettled CCLC work");
    std::vector<ControlDomainId> domains;
    for (const auto& route : compiled) domains.push_back(route.control_domain_id);
    RequireNativeApplyCapacityLocked(components, domains);
    Exec(database_, "DELETE FROM cclc_native_apply_plan_components");
    Exec(database_, "DELETE FROM cclc_native_apply_plan_routes");
    Exec(database_, "DELETE FROM cclc_native_apply_plan_removals");
    Exec(database_, "DELETE FROM cclc_native_apply_plans");
    Statement insert(database_,
                     "INSERT INTO cclc_native_apply_plans(singleton,apply_id,apply_generation,"
                     "candidate_bundle_id,candidate_graph_revision,component_bring_up) VALUES(1,?,?,?,?,?)");
    insert.Bind(1, request.apply.apply_id.value());
    insert.Bind(2, static_cast<std::int64_t>(request.apply.apply_generation.value()));
    insert.Bind(3, request.candidate_retained_bundle_id.value());
    insert.Bind(4, request.candidate_graph_revision.value());
    if (link_blob) insert.BindBlob(5, *link_blob);
    else insert.BindNull(5);
    Require(!insert.Step(), "component_lifecycle_journal_corrupt",
            "native Apply plan write returned a row");
    const auto reserve = [&](std::string_view kind, std::string_view identity,
                             std::uint64_t generation) {
      Statement row(database_,
                    "INSERT INTO cclc_native_apply_reservations("
                    "reservation_kind,identity,generation) VALUES(?,?,?) "
                    "ON CONFLICT(reservation_kind,identity) DO UPDATE SET "
                    "generation=excluded.generation");
      row.Bind(1, kind);
      row.Bind(2, identity);
      row.Bind(3, static_cast<std::int64_t>(generation));
      Require(!row.Step(), "component_lifecycle_journal_corrupt",
              "native Apply reservation write returned a row");
    };
    for (const auto& component : components) {
      std::optional<std::pair<ComponentLifecycleStateDTO, std::uint64_t>> current_fact;
      {
        Statement current(database_,
                          "SELECT component_lifecycle_state,component_lifecycle_generation "
                          "FROM lifecycle_component_current_facts "
                          "WHERE controllable_component_id=?");
        current.Bind(1, component.value());
        if (current.Step()) {
          current_fact.emplace(static_cast<ComponentLifecycleStateDTO>(current.Integer(0)),
                               Positive(current.Integer(1), "current component generation"));
        }
      }
      const bool retained =
          std::binary_search(retained_components.begin(), retained_components.end(), component);
      auto state = request.changed_component_state;
      std::uint64_t generation = 0U;
      const auto delegated = children.find(component);
      if (delegated != children.end()) {
        // Acceptance is the sole needed-component generation writer. The
        // native plan records that receipt and requested state without a reservation.
        generation = delegated->second.acceptance.component_lifecycle_generation.value();
        state = *delegated->second.requested_state.state;
      } else if (retained) {
        // The retained runtime keeps its exact lifecycle fact. A component
        // with no coherent current state (e.g. after a settled partial
        // effect) cannot be retained: that needs restoration, not reuse.
        Require(current_fact.has_value(), "component_lifecycle_native_apply_retained_fact_missing",
                "native Apply retains " + component.value() +
                    " but it has no coherent current lifecycle state");
        state = current_fact->first;
        generation = current_fact->second;
      } else {
        // Only an explicit, completed Disable is preserved; every other
        // changed component takes the requested state (enabled by a native
        // Apply, only declared for an externally managed controller).
        if (current_fact.has_value() &&
            current_fact->first == ComponentLifecycleStateDTO::kDisabled) {
          state = ComponentLifecycleStateDTO::kDisabled;
        }
        generation = SelectApplyComponentGenerationLocked(component.value()).candidate_generation.value();
        reserve("component", component.value(), generation);
      }
      Statement row(database_,
                    "INSERT INTO cclc_native_apply_plan_components(controllable_component_id,"
                    "component_lifecycle_state,component_lifecycle_generation,retained) "
                    "VALUES(?,?,?,?)");
      row.Bind(1, component.value());
      row.Bind(2, static_cast<std::int64_t>(state));
      row.Bind(3, static_cast<std::int64_t>(generation));
      row.Bind(4, static_cast<std::int64_t>(retained ? 1 : 0));
      Require(!row.Step(), "component_lifecycle_journal_corrupt",
              "native Apply plan component write returned a row");
    }
    for (const auto& route : compiled) {
      std::uint64_t generation = 0U;
      const auto retained = retained_routes.find(route.control_domain_id);
      if (retained != retained_routes.end()) {
        // The live route the host keeps must be exactly the journal's
        // current fact; retaining it mints and reserves nothing.
        Statement current(database_,
                          "SELECT configuration_blob FROM control_domain_active_configurations "
                          "WHERE control_domain_id=?");
        current.Bind(1, route.control_domain_id.value());
        Require(current.Step() &&
                    detail::DecodeActiveControlConfiguration(current.Blob(0)) ==
                        retained->second &&
                    !current.Step(),
                "component_lifecycle_native_apply_retained_route_mismatch",
                "native Apply retained route " + route.control_domain_id.value() +
                    " differs from the journal's current configuration");
        generation = retained->second.active_control_configuration_generation.value();
      } else {
        const auto floor = NativeApplyGenerationFloorLocked(
            "route", route.control_domain_id.value());
        Require(FitsSqliteInteger(floor + 1U),
                "component_lifecycle_native_apply_generation_exhausted",
                "native Apply route generation cannot advance");
        // First publication keeps the compiled generation; afterwards the
        // route continues past every used or reserved generation.
        generation =
            floor == 0U ? route.generation.value() : std::max(route.generation.value(), floor + 1U);
        reserve("route", route.control_domain_id.value(), generation);
      }
      Statement row(database_,
                    "INSERT INTO cclc_native_apply_plan_routes(control_domain_id,"
                    "active_control_configuration_generation,retained) VALUES(?,?,?)");
      row.Bind(1, route.control_domain_id.value());
      row.Bind(2, static_cast<std::int64_t>(generation));
      row.Bind(3, static_cast<std::int64_t>(retained != retained_routes.end() ? 1 : 0));
      Require(!row.Step(), "component_lifecycle_journal_corrupt",
              "native Apply plan route write returned a row");
    }
    const auto record_removal = [&](std::string_view kind, std::string_view identity) {
      Statement row(database_,
                    "INSERT INTO cclc_native_apply_plan_removals(removal_kind,identity) "
                    "VALUES(?,?)");
      row.Bind(1, kind);
      row.Bind(2, identity);
      Require(!row.Step(), "component_lifecycle_journal_corrupt",
              "native Apply plan removal write returned a row");
    };
    for (const auto& component : removed_components) {
      record_removal("component", component.value());
    }
    for (const auto& domain : removed_domains) record_removal("route", domain.value());
  }
  auto plan = ReadNativeApplyPlanLocked(request.apply);
  Require(plan.has_value(), "component_lifecycle_journal_corrupt",
          "native Apply plan is not readable after it was recorded");
  std::vector<ControlDomainId> planned_domains;
  for (const auto& route : plan->routes) planned_domains.push_back(route.control_domain_id);
  std::vector<ControlDomainId> requested_domains;
  for (const auto& route : compiled) requested_domains.push_back(route.control_domain_id);
  std::vector<ControllableComponentId> planned_components;
  for (const auto& component : plan->components) planned_components.push_back(component.component_id);
  Require(planned_domains == requested_domains && planned_components == components &&
              plan->retained_components == retained_components &&
              plan->retained_domains == retained_domains &&
              plan->removed_components == removed_components &&
              plan->removed_domains == removed_domains &&
              plan->component_bring_up == request.component_bring_up &&
              std::all_of(plan->components.begin(), plan->components.end(),
                          [&](const auto& component) {
                            return (request.component_bring_up &&
                                    std::any_of(request.component_bring_up->children.begin(),
                                                request.component_bring_up->children.end(), [&](const auto& child) {
                                      return child.component_id == component.component_id;
                                    })) ||
                                   std::binary_search(retained_components.begin(),
                                                      retained_components.end(),
                                                      component.component_id) ||
                                   component.state == ComponentLifecycleStateDTO::kDisabled ||
                                   component.state == request.changed_component_state;
                          }),
          "component_lifecycle_native_apply_conflict",
          "native Apply replay names another component, route, retained or removed scope, "
          "or another changed component state");
  transaction.Commit();
  return *plan;
}

std::optional<NativeApplyPlanDTO> SqliteCCLCJournal::Impl::LoadNativeApplyPlan(
    const NamedApplyPublicationTransactionDTO& apply) {
  Require(apply.valid(), "component_lifecycle_native_apply_invalid",
          "native Apply plan query has no exact Apply");
  std::lock_guard lock(mutex_);
  RequireOpen();
  return ReadNativeApplyPlanLocked(apply);
}

void SqliteCCLCJournal::Impl::AdoptNativeApplyFacts(
    const NativeApplyFactAdoptionDTO& adoption) {

  const auto* named_apply = std::get_if<NamedApplyPublicationTransactionDTO>(
      &adoption.publication.request.transaction_ref);
  Require(adoption.publication.valid() && named_apply != nullptr &&
              adoption.components.size() <= kMaximumCCLCComponentWatermarks &&
              adoption.current_routes.size() <=
                  kMaximumCCLCActiveConfigurationWatermarks,
          "component_lifecycle_native_apply_invalid",
          "native Apply adoption has an invalid publication or scope");
  auto components = adoption.components;
  std::sort(components.begin(), components.end(), [](const auto& a, const auto& b) {
    return a.component_id < b.component_id;
  });
  auto routes = adoption.current_routes;
  std::sort(routes.begin(), routes.end(), [](const auto& a, const auto& b) {
    return a.control_domain_id < b.control_domain_id;
  });
  std::string facts;
  const auto append = [&](std::string_view value) {
    facts += std::to_string(value.size());
    facts += ':';
    facts.append(value);
  };
  append(adoption.publication.request.candidate_retained_bundle_id.value());
  append(adoption.publication.request.candidate_graph_revision.value());
  for (std::size_t index = 0U; index < components.size(); ++index) {
    const auto& component = components[index];
    // The recorded plan fixes each state (a retained component may keep any
    // known state); here only the shape is checked.
    Require(!component.component_id.empty() && !to_string(component.state).empty() &&
                component.generation.nonzero() &&
                FitsSqliteInteger(component.generation.value()) &&
                (index == 0U || components[index - 1U].component_id != component.component_id),
            "component_lifecycle_native_apply_invalid",
            "native Apply component facts are invalid or duplicated");
    append(component.component_id.value());
    append(to_string(component.state));
    append(std::to_string(component.generation.value()));
  }
  for (std::size_t index = 0U; index < routes.size(); ++index) {
    const auto& route = routes[index];
    Require(route.locally_valid() &&
                route.state == ActiveControlConfigurationStateDTO::kReconciled &&
                FitsSqliteInteger(route.active_control_configuration_generation.value()) &&
                (index == 0U || routes[index - 1U].control_domain_id != route.control_domain_id),
            "component_lifecycle_native_apply_invalid",
            "native Apply route facts are invalid or duplicated");
    append(route.control_domain_id.value());
    const auto encoded = detail::EncodeActiveControlConfiguration(route);
    append(std::string_view(reinterpret_cast<const char*>(encoded.data()),
                            encoded.size()));
  }
  if (adoption.component_bring_up) {
    append(detail::EncodeNativeApplyComponentBringUp(*adoption.component_bring_up));
  }
  const auto facts_digest = content_identity::sha256_digest(facts);

  std::lock_guard lock(mutex_);
  RequireOpen();
  Transaction transaction(database_);
  Statement prior(database_,
                  "SELECT publication_generation,public_pointer_digest,"
                  "candidate_bundle_id,candidate_graph_revision,facts_digest "
                  "FROM cclc_native_apply_adoptions WHERE apply_id=? AND apply_generation=?");
  prior.Bind(1, named_apply->apply_id.value());
  prior.Bind(2, static_cast<std::int64_t>(named_apply->apply_generation.value()));
  if (prior.Step()) {
    Require(Positive(prior.Integer(0), "native Apply publication generation") ==
                    adoption.publication.publication_generation.value() &&
                prior.Text(1) ==
                    adoption.publication.durable_public_pointer_content_digest.value() &&
                prior.Text(2) ==
                    adoption.publication.request.candidate_retained_bundle_id.value() &&
                prior.Text(3) ==
                    adoption.publication.request.candidate_graph_revision.value() &&
                prior.Text(4) == facts_digest && !prior.Step(),
            "component_lifecycle_native_apply_conflict",
            "native Apply was already promoted with different facts");
    transaction.Commit();
    return;
  }
  // Only the facts CCLC planned before publication may be adopted.
  const auto plan = ReadNativeApplyPlanLocked(*named_apply);
  Require(plan.has_value() &&
              plan->candidate_retained_bundle_id ==
                  adoption.publication.request.candidate_retained_bundle_id &&
              plan->candidate_graph_revision ==
                  adoption.publication.request.candidate_graph_revision &&
              plan->components.size() == components.size() &&
              plan->routes.size() == routes.size() &&
              plan->component_bring_up == adoption.component_bring_up,
          "component_lifecycle_native_apply_unplanned",
          "native Apply facts have no matching recorded plan");
  for (std::size_t index = 0U; index < components.size(); ++index) {
    Require(plan->components[index].component_id == components[index].component_id &&
                plan->components[index].state == components[index].state &&
                plan->components[index].generation == components[index].generation,
            "component_lifecycle_native_apply_unplanned",
            "native Apply component facts differ from the recorded plan");
  }
  for (std::size_t index = 0U; index < routes.size(); ++index) {
    Require(plan->routes[index].control_domain_id == routes[index].control_domain_id &&
                plan->routes[index].generation ==
                    routes[index].active_control_configuration_generation,
            "component_lifecycle_native_apply_unplanned",
            "native Apply route generations differ from the recorded plan");
  }
  std::map<ControllableComponentId, ComponentLifecycleStatusDTO> children;
  if (adoption.component_bring_up) {
    children = ReadNativeApplyChildrenLocked(*adoption.component_bring_up, *named_apply,
        plan->candidate_retained_bundle_id, plan->candidate_graph_revision, false, &adoption.publication);
    for (const auto& [component, child] : children) {
      const auto fact = std::find_if(components.begin(), components.end(),
          [&](const auto& row) { return row.component_id == component; });
      Require(fact != components.end() && fact->state == child.current_state &&
                  fact->generation == child.acceptance.component_lifecycle_generation,
              "component_lifecycle_native_apply_unplanned", "native Apply child settled state differs from adoption");
      for (const auto& domain : child.domains) {
        if (!domain.expected_domain) continue;
        const auto route = std::find_if(routes.begin(), routes.end(),
            [&](const auto& row) { return row.control_domain_id == domain.control_domain_id; });
        Require(route != routes.end() &&
                    route->control_domain_revision == domain.current_scope->control_domain_revision &&
                    route->controller_topology_id == domain.current_scope->controller_topology_id &&
                    route->topology_revision == domain.current_scope->topology_revision &&
                    route->source_runtime_unit_id == domain.resolved_provider_occurrence->provider_runtime_unit_id &&
                    route->source_runtime_epoch == domain.resolved_provider_occurrence->provider_runtime_epoch &&
                    route->binding_routes.size() == domain.current_scope->current_control_bindings.size(),
                "component_lifecycle_native_apply_unplanned", "native Apply route differs from actual installed child");
        for (std::size_t index = 0U; index < route->binding_routes.size(); ++index) {
          const auto& actual = route->binding_routes[index];
          const auto& expected = domain.current_scope->current_control_bindings[index];
          Require(actual.control_binding_id == expected.control_binding_id &&
                      actual.control_binding_revision == expected.control_binding_revision &&
                      actual.control_route_id == expected.control_route_id,
                  "component_lifecycle_native_apply_unplanned", "native Apply route binding differs from installed child");
        }
      }
    }
  }
  Require(ActiveTransactionCount() == 0U &&
              ScalarInteger(database_,
                  "SELECT count(*) FROM cclc_lifecycle_cell_claims "
                  "WHERE release_receipt_recorded=0") == 0,
          "component_lifecycle_native_apply_unsettled",
          "native Apply cannot replace unsettled CCLC facts");
  std::vector<ControllableComponentId> component_ids;
  for (const auto& component : components) component_ids.push_back(component.component_id);
  std::vector<ControlDomainId> domain_ids;
  for (const auto& route : routes) domain_ids.push_back(route.control_domain_id);
  RequireNativeApplyCapacityLocked(component_ids, domain_ids);

  // Retained facts stay exactly as they are; changed facts advance; every
  // other current fact (removed or no longer native) is erased while its
  // watermark stays, so a later re-add always mints a newer generation.
  const auto retained_component = [&](const ControllableComponentId& id) {
    return std::binary_search(plan->retained_components.begin(),
                              plan->retained_components.end(), id);
  };
  const auto retained_domain = [&](const ControlDomainId& id) {
    return std::binary_search(plan->retained_domains.begin(), plan->retained_domains.end(), id);
  };
  const auto watermark_of = [&](std::string_view kind, std::string_view identity) {
    Statement old(database_,
                  kind == "component"
                      ? "SELECT component_lifecycle_generation FROM "
                        "lifecycle_component_watermarks WHERE controllable_component_id=?"
                      : "SELECT active_control_configuration_generation FROM "
                        "control_domain_active_configuration_watermarks "
                        "WHERE control_domain_id=?");
    old.Bind(1, identity);
    const auto previous = old.Step() ? Positive(old.Integer(0), "native Apply watermark") : 0U;
    Require(!old.Step(), "component_lifecycle_journal_corrupt",
            "native Apply watermark is duplicated");
    return previous;
  };
  const auto advance = [&](std::string_view kind, std::string_view identity,
                           std::uint64_t generation) {
    // The planned generation is above every earlier one; the watermark and
    // current fact move to it together, and its reservation is released.
    Require(watermark_of(kind, identity) < generation,
            "component_lifecycle_native_apply_generation_stale",
            "native Apply planned generation is no longer above the journal watermark");
    Statement watermark(database_,
                        kind == "component"
                            ? "INSERT INTO lifecycle_component_watermarks("
                              "controllable_component_id,component_lifecycle_generation) "
                              "VALUES(?,?) ON CONFLICT(controllable_component_id) DO UPDATE SET "
                              "component_lifecycle_generation=excluded.component_lifecycle_generation"
                            : "INSERT INTO control_domain_active_configuration_watermarks("
                              "control_domain_id,active_control_configuration_generation) "
                              "VALUES(?,?) ON CONFLICT(control_domain_id) DO UPDATE SET "
                              "active_control_configuration_generation="
                              "excluded.active_control_configuration_generation");
    watermark.Bind(1, identity);
    watermark.Bind(2, static_cast<std::int64_t>(generation));
    Require(!watermark.Step(), "component_lifecycle_journal_corrupt",
            "native Apply watermark write returned a row");
    Statement release(database_,
                      "DELETE FROM cclc_native_apply_reservations "
                      "WHERE reservation_kind=? AND identity=? AND generation<=?");
    release.Bind(1, kind);
    release.Bind(2, identity);
    release.Bind(3, static_cast<std::int64_t>(generation));
    Require(!release.Step(), "component_lifecycle_journal_corrupt",
            "native Apply reservation release returned a row");
  };

  // Verify every retained fact before any write: the plan retained exactly
  // what was current then, and nothing may have changed it since.
  for (const auto& component : components) {
    if (!retained_component(component.component_id)) continue;
    Statement current(database_,
                      "SELECT component_lifecycle_state,component_lifecycle_generation "
                      "FROM lifecycle_component_current_facts WHERE controllable_component_id=?");
    current.Bind(1, component.component_id.value());
    Require(current.Step() &&
                static_cast<ComponentLifecycleStateDTO>(current.Integer(0)) == component.state &&
                Positive(current.Integer(1), "retained component generation") ==
                    component.generation.value() &&
                !current.Step() &&
                watermark_of("component", component.component_id.value()) ==
                    component.generation.value(),
            "component_lifecycle_native_apply_retained_fact_changed",
            "native Apply retained component " + component.component_id.value() +
                " no longer has its planned fact");
  }
  for (const auto& route : routes) {
    if (!retained_domain(route.control_domain_id)) continue;
    Statement current(database_,
                      "SELECT configuration_blob FROM control_domain_active_configurations "
                      "WHERE control_domain_id=?");
    current.Bind(1, route.control_domain_id.value());
    Require(current.Step() && detail::DecodeActiveControlConfiguration(current.Blob(0)) == route &&
                !current.Step(),
            "component_lifecycle_native_apply_retained_fact_changed",
            "native Apply retained route " + route.control_domain_id.value() +
                " is not the journal's current configuration");
  }

  // Erase current facts outside the planned sets (removed components and
  // domains, or ones no longer native) without touching their watermarks.
  {
    std::vector<std::string> stale;
    Statement current(database_,
                      "SELECT controllable_component_id FROM lifecycle_component_current_facts");
    while (current.Step()) {
      const ControllableComponentId id(current.Text(0));
      if (std::none_of(components.begin(), components.end(),
                       [&](const auto& row) { return row.component_id == id; })) {
        stale.push_back(id.value());
      }
    }
    for (const auto& id : stale) {
      Statement erase(database_,
                      "DELETE FROM lifecycle_component_current_facts "
                      "WHERE controllable_component_id=?");
      erase.Bind(1, id);
      Require(!erase.Step(), "component_lifecycle_journal_corrupt",
              "native Apply component fact removal returned a row");
    }
  }
  {
    std::vector<std::string> stale;
    Statement current(database_,
                      "SELECT control_domain_id FROM control_domain_active_configurations");
    while (current.Step()) {
      const ControlDomainId id(current.Text(0));
      if (std::none_of(routes.begin(), routes.end(),
                       [&](const auto& row) { return row.control_domain_id == id; })) {
        stale.push_back(id.value());
      }
    }
    for (const auto& id : stale) {
      Statement erase(database_,
                      "DELETE FROM control_domain_active_configurations WHERE control_domain_id=?");
      erase.Bind(1, id);
      Require(!erase.Step(), "component_lifecycle_journal_corrupt",
              "native Apply route fact removal returned a row");
    }
  }

  for (const auto& component : components) {
    if (retained_component(component.component_id)) continue;
    if (children.contains(component.component_id)) {
      Statement release(database_, "DELETE FROM cclc_native_apply_reservations "
          "WHERE reservation_kind='component' AND identity=? AND generation<=?");
      release.Bind(1, component.component_id.value());
      release.Bind(2, static_cast<std::int64_t>(component.generation.value()));
      Require(!release.Step(), "component_lifecycle_journal_corrupt",
              "native Apply linked reservation release returned a row");
      continue;
    }
    advance("component", component.component_id.value(), component.generation.value());
    Statement current(database_,
        "INSERT INTO lifecycle_component_current_facts("
        "controllable_component_id,component_lifecycle_generation,"
        "component_lifecycle_state) VALUES(?,?,?) "
        "ON CONFLICT(controllable_component_id) DO UPDATE SET "
        "component_lifecycle_generation=excluded.component_lifecycle_generation,"
        "component_lifecycle_state=excluded.component_lifecycle_state");
    current.Bind(1, component.component_id.value());
    current.Bind(2, static_cast<std::int64_t>(component.generation.value()));
    current.Bind(3, static_cast<std::int64_t>(component.state));
    Require(!current.Step(), "component_lifecycle_journal_corrupt",
            "native Apply component fact write returned a row");
  }
  for (const auto& route : routes) {
    if (retained_domain(route.control_domain_id)) continue;
    advance("route", route.control_domain_id.value(),
            route.active_control_configuration_generation.value());
    Statement current(database_,
        "INSERT INTO control_domain_active_configurations("
        "control_domain_id,active_control_configuration_generation,"
        "origin_route_selection_transaction_id,"
        "origin_route_selection_transaction_generation,configuration_blob) "
        "VALUES(?,?,NULL,NULL,?) "
        "ON CONFLICT(control_domain_id) DO UPDATE SET "
        "active_control_configuration_generation="
        "excluded.active_control_configuration_generation,"
        "origin_route_selection_transaction_id=NULL,"
        "origin_route_selection_transaction_generation=NULL,"
        "configuration_blob=excluded.configuration_blob");
    current.Bind(1, route.control_domain_id.value());
    current.Bind(2, static_cast<std::int64_t>(
                        route.active_control_configuration_generation.value()));
    current.BindBlob(3, detail::EncodeActiveControlConfiguration(route));
    Require(!current.Step(), "component_lifecycle_journal_corrupt",
            "native Apply route fact write returned a row");
  }
  // Configuration-only M adoption never manufactures a followed target.
  // Unchanged components retain their exact target; new/changed components
  // retire only requested intent, without Recover/Reset or cause cleanup.
  // The unwired linked branch retains its earlier target semantics. Actual
  // component removal remains the existing separate cancellation disposition.
  {
    std::vector<ControllableComponentId> stale;
    Statement present(database_,
                      "SELECT controllable_component_id FROM cclc_component_lifecycle_targets "
                      "WHERE present=1");
    while (present.Step()) {
      const ControllableComponentId id(present.Text(0));
      if (std::none_of(components.begin(), components.end(),
                       [&](const auto& row) { return row.component_id == id; })) {
        stale.push_back(id);
      }
    }
    for (const auto& id : stale) CancelLifecycleTargetLocked(id);
  }
  for (const auto& component : components) {
    const auto existing = ReadLifecycleTargetLocked(component.component_id);
    const bool has_target = existing.has_value() && existing->second;
    if (!plan->component_bring_up) {
      if (!retained_component(component.component_id) && has_target)
        RetireLifecycleTargetIntentLocked(component.component_id);
      continue;
    }
    if (retained_component(component.component_id) && has_target) continue;
    // A linked child's installed poststate is a step result, not a new target.
    if (children.contains(component.component_id) && has_target) continue;
    if (component.state == ComponentLifecycleStateDTO::kDisabled) {
      if (has_target && existing->first.requested_state == LifecycleTargetStateDTO::kDisabled) {
        continue;
      }
      static_cast<void>(WriteLifecycleTargetLocked(
          component.component_id, LifecycleTargetStateDTO::kDisabled,
          LifecycleTargetOriginDTO::kOperator, true));
    } else {
      static_cast<void>(WriteLifecycleTargetLocked(component.component_id,
                                                   LifecycleTargetStateDTO::kActive,
                                                   LifecycleTargetOriginDTO::kStartup, true));
    }
  }
  Statement retain(database_,
      "INSERT INTO cclc_native_apply_adoptions("
      "apply_id,apply_generation,publication_generation,public_pointer_digest,"
      "candidate_bundle_id,candidate_graph_revision,facts_digest) "
      "VALUES(?,?,?,?,?,?,?)");
  retain.Bind(1, named_apply->apply_id.value());
  retain.Bind(2, static_cast<std::int64_t>(named_apply->apply_generation.value()));
  retain.Bind(3, static_cast<std::int64_t>(
                     adoption.publication.publication_generation.value()));
  retain.Bind(4, adoption.publication.durable_public_pointer_content_digest.value());
  retain.Bind(5, adoption.publication.request.candidate_retained_bundle_id.value());
  retain.Bind(6, adoption.publication.request.candidate_graph_revision.value());
  retain.Bind(7, facts_digest);
  Require(!retain.Step(), "component_lifecycle_journal_corrupt",
          "native Apply adoption write returned a row");
  transaction.Commit();
}

bool SqliteCCLCJournal::Impl::HasNativeApplyFacts(
    const ActiveBundlePublicationReceiptDTO& publication) {
  const auto* named_apply = std::get_if<NamedApplyPublicationTransactionDTO>(
      &publication.request.transaction_ref);
  Require(publication.valid() && named_apply != nullptr,
          "component_lifecycle_native_apply_invalid",
          "native Apply fact query has no exact named publication");
  std::lock_guard lock(mutex_);
  RequireOpen();
  Statement row(database_,
                "SELECT publication_generation,public_pointer_digest,"
                "candidate_bundle_id,candidate_graph_revision "
                "FROM cclc_native_apply_adoptions "
                "WHERE apply_id=? AND apply_generation=?");
  row.Bind(1, named_apply->apply_id.value());
  row.Bind(2, static_cast<std::int64_t>(named_apply->apply_generation.value()));
  if (!row.Step()) return false;
  Require(Positive(row.Integer(0), "native Apply publication generation") ==
                  publication.publication_generation.value() &&
              row.Text(1) ==
                  publication.durable_public_pointer_content_digest.value() &&
              row.Text(2) ==
                  publication.request.candidate_retained_bundle_id.value() &&
              row.Text(3) ==
                  publication.request.candidate_graph_revision.value() &&
              !row.Step(),
          "component_lifecycle_native_apply_conflict",
          "native Apply adoption conflicts with the active publication");
  return true;
}

std::optional<std::pair<ComponentLifecycleTargetDTO, bool>>
SqliteCCLCJournal::Impl::ReadLifecycleTargetLocked(const ControllableComponentId& component) {
  auto retained = ReadLifecycleTargetRetentionLocked(component);
  if (!retained) return std::nullopt;
  return std::pair{std::move(retained->target), retained->present};
}

std::optional<ComponentLifecycleTargetRetentionDTO>
SqliteCCLCJournal::Impl::ReadLifecycleTargetRetentionLocked(const ControllableComponentId& component) {
  Statement row(database_,
                "SELECT requested_state,origin,target_generation,present,reached,recovered_cause,retirement_reason "
                "FROM cclc_component_lifecycle_targets WHERE controllable_component_id=?");
  row.Bind(1, component.value());
  if (!row.Step()) return std::nullopt;
  Require(row.Integer(6) >= 0 && row.Integer(6) <= 2 &&
              (row.Integer(3) == 0 || row.Integer(3) == 1),
          "component_lifecycle_journal_corrupt", "retained target retirement discriminator is invalid");
  const ComponentLifecycleTargetDTO target{
      component,
      static_cast<LifecycleTargetStateDTO>(row.Integer(0)),
      static_cast<LifecycleTargetOriginDTO>(row.Integer(1)),
      Positive(row.Integer(2), "lifecycle target generation"),
      row.Integer(4) == 1,
      row.IsNull(5) ? std::nullopt
                    : std::optional(static_cast<LifecycleBlockingCauseClassDTO>(row.Integer(5)))};
  const ComponentLifecycleTargetRetentionDTO retained{
      target, row.Integer(3) == 1,
      static_cast<LifecycleTargetRetirementReasonDTO>(row.Integer(6))};
  Require(retained.locally_valid() && !row.Step(), "component_lifecycle_journal_corrupt",
          "lifecycle target row is invalid or duplicated");
  return retained;
}

std::optional<ComponentLifecycleTargetRetentionDTO>
SqliteCCLCJournal::Impl::ReadLifecycleTargetRetention(const ControllableComponentId& component) {
  Require(!component.empty() && component.value().size() <= kMaxLifecycleIdentityBytes,
          "component_lifecycle_journal_query_invalid", "retained target identity is invalid");
  std::lock_guard lock(mutex_);
  RequireOpen();
  return ReadLifecycleTargetRetentionLocked(component);
}

ComponentLifecycleTargetDTO SqliteCCLCJournal::Impl::WriteLifecycleTargetLocked(
    const ControllableComponentId& component, const LifecycleTargetStateDTO requested_state,
    const LifecycleTargetOriginDTO origin, const bool reset_reached) {
  const auto previous = ReadLifecycleTargetLocked(component);
  const std::uint64_t previous_generation = previous ? previous->first.target_generation : 0U;
  Require(FitsSqliteInteger(previous_generation + 1U), "component_lifecycle_journal_capacity",
          "lifecycle target generation is exhausted");
  const bool keep = !reset_reached && previous.has_value() && previous->second;
  const ComponentLifecycleTargetDTO target{
      component,
      requested_state,
      origin,
      previous_generation + 1U,
      keep && previous->first.reached,
      keep ? previous->first.recovered_cause : std::nullopt};
  Require(target.locally_valid(), "component_lifecycle_target_invalid",
          "lifecycle target is invalid");
  Statement write(database_,
                  "INSERT INTO cclc_component_lifecycle_targets("
                  "controllable_component_id,requested_state,origin,target_generation,present,"
                  "reached,recovered_cause,retirement_reason) VALUES(?,?,?,?,1,?,?,0) "
                  "ON CONFLICT(controllable_component_id) DO UPDATE "
                  "SET requested_state=excluded.requested_state,origin=excluded.origin,"
                  "target_generation=excluded.target_generation,present=1,"
                  "reached=excluded.reached,recovered_cause=excluded.recovered_cause,retirement_reason=0");
  write.Bind(1, component.value());
  write.Bind(2, static_cast<std::int64_t>(requested_state));
  write.Bind(3, static_cast<std::int64_t>(origin));
  write.Bind(4, static_cast<std::int64_t>(target.target_generation));
  write.Bind(5, static_cast<std::int64_t>(target.reached ? 1 : 0));
  if (target.recovered_cause) {
    write.Bind(6, static_cast<std::int64_t>(*target.recovered_cause));
  } else {
    write.BindNull(6);
  }
  Require(!write.Step(), "component_lifecycle_journal_corrupt",
          "lifecycle target write returned a row");
  return target;
}

void SqliteCCLCJournal::Impl::RetireLifecycleTargetIntentLocked(
    const ControllableComponentId& component, LifecycleTargetRetirementReasonDTO reason) {
  Require(reason == LifecycleTargetRetirementReasonDTO::kOrdinaryCancellation ||
              reason == LifecycleTargetRetirementReasonDTO::kClose,
          "component_lifecycle_journal_record_invalid", "target retirement reason is invalid");
  Statement retire(database_,
                   "UPDATE cclc_component_lifecycle_targets SET present=0,reached=0,retirement_reason=? "
                   "WHERE controllable_component_id=? AND present=1");
  retire.Bind(1, static_cast<std::int64_t>(reason));
  retire.Bind(2, component.value());
  Require(!retire.Step(), "component_lifecycle_journal_corrupt",
          "lifecycle target intent retirement returned a row");
}

void SqliteCCLCJournal::Impl::CancelLifecycleTargetLocked(
    const ControllableComponentId& component) {
  Statement cancel(database_,
                   "UPDATE cclc_component_lifecycle_targets SET present=0,reached=0,"
                   "recovered_cause=NULL,retirement_reason=CASE WHEN retirement_reason=2 THEN 2 ELSE 1 END "
                   "WHERE controllable_component_id=?");
  cancel.Bind(1, component.value());
  Require(!cancel.Step(), "component_lifecycle_journal_corrupt",
          "lifecycle target cancellation returned a row");
  Statement causes(database_,
                   "DELETE FROM cclc_component_intervention_causes "
                   "WHERE controllable_component_id=?");
  causes.Bind(1, component.value());
  Require(!causes.Step(), "component_lifecycle_journal_corrupt",
          "intervention cause removal returned a row");
  AdvanceRecoveryGenerationLocked(component);
}

std::uint64_t SqliteCCLCJournal::Impl::ReadRecoveryGenerationLocked(
    const ControllableComponentId& component) {
  Statement row(database_,
                "SELECT recovery_generation FROM cclc_component_recovery_generations "
                "WHERE controllable_component_id=?");
  row.Bind(1, component.value());
  if (!row.Step()) return 0U;
  const auto generation = Positive(row.Integer(0), "component recovery generation");
  Require(!row.Step(), "component_lifecycle_journal_corrupt",
          "component recovery generation is duplicated");
  return generation;
}

void SqliteCCLCJournal::Impl::AdvanceRecoveryGenerationLocked(
    const ControllableComponentId& component) {
  const auto next = ReadRecoveryGenerationLocked(component) + 1U;
  Require(FitsSqliteInteger(next), "component_lifecycle_journal_capacity",
          "component recovery generation is exhausted");
  Statement write(database_,
                  "INSERT INTO cclc_component_recovery_generations("
                  "controllable_component_id,recovery_generation) VALUES(?,?) "
                  "ON CONFLICT(controllable_component_id) DO UPDATE SET "
                  "recovery_generation=excluded.recovery_generation");
  write.Bind(1, component.value());
  write.Bind(2, static_cast<std::int64_t>(next));
  Require(!write.Step(), "component_lifecycle_journal_corrupt",
          "component recovery generation write returned a row");
}

void SqliteCCLCJournal::Impl::RequireRecoveryGenerationLocked(
    const ControllableComponentId& component, const std::uint64_t expected) {
  Require(ReadRecoveryGenerationLocked(component) == expected,
          std::string(kLifecycleFactsStaleErrorCode),
          "component " + component.value() +
              " was recovered or its target cancelled after the facts this write is based on "
              "were read; nothing was written");
}

ComponentLifecycleTargetDTO SqliteCCLCJournal::Impl::SetLifecycleTarget(
    const ControllableComponentId& component, const LifecycleTargetStateDTO requested_state,
    const LifecycleTargetOriginDTO origin) {
  Require(!component.empty() && component.value().size() <= kMaxLifecycleIdentityBytes &&
              !to_string(requested_state).empty() && !to_string(origin).empty(),
          "component_lifecycle_target_invalid", "lifecycle target request is invalid");
  std::lock_guard lock(mutex_);
  RequireOpen();
  Transaction transaction(database_);
  // Only a component whose facts an Apply adopted can carry a target.
  Statement current(database_,
                    "SELECT 1 FROM lifecycle_component_current_facts "
                    "WHERE controllable_component_id=?");
  current.Bind(1, component.value());
  Require(current.Step(), "component_lifecycle_target_unknown_component",
          "component " + component.value() + " has no adopted lifecycle fact");
  auto target = WriteLifecycleTargetLocked(component, requested_state, origin, false);
  transaction.Commit();
  return target;
}

std::vector<ComponentLifecycleTargetDTO> SqliteCCLCJournal::Impl::LoadLifecycleTargets() {
  std::lock_guard lock(mutex_);
  RequireOpen();
  return LoadLifecycleTargetsLocked();
}

std::vector<ComponentLifecycleTargetDTO> SqliteCCLCJournal::Impl::LoadLifecycleTargetsForClose(
    const RequestOwnerEpoch& owner) {
  Require(!owner.empty() && owner.value().size() <= kMaxLifecycleIdentityBytes,
          "component_lifecycle_journal_query_invalid", "Close owner identity is invalid");
  std::lock_guard lock(mutex_);
  RequireOpen();
  ReadTransaction transaction(database_);
  Require(ActiveOwner(owner), "stale_request_owner_epoch", "Close target offer owner changed");
  auto rows = LoadLifecycleTargetsLocked();
  transaction.Commit();
  return rows;
}

std::optional<CCLCLifecycleTargetRetirementResultDTO>
SqliteCCLCJournal::Impl::FindTargetRetirementResultLocked(
    const CCLCLifecycleTargetRetirementIntentDTO& intent, const std::string_view digest) {
  Statement row(database_, "SELECT request_kind,semantic_request_digest,retained_result,"
                           "target_retirement_blob FROM cclc_request_memberships "
                           "WHERE owner_epoch=? AND request_id=?");
  row.Bind(1, intent.request_context.request_owner_epoch.value());
  row.Bind(2, intent.request_context.request_id.value());
  if (!row.Step()) return std::nullopt;
  Require(row.Text(0) == "lifecycle_target_retirement" && row.Text(1) == digest,
          "request_id_conflict", "Close retirement request identity has different original bytes");
  Require(row.Integer(2) == 1, "request_result_expired", "original Close target retirement result expired");
  Require(!row.IsNull(3), "component_lifecycle_journal_corrupt", "retirement result blob is missing");
  const auto record = detail::DecodeCCLCTargetRetirementRecord(row.Blob(3));
  Require(record.intent == intent && !row.Step(), "component_lifecycle_journal_corrupt",
          "retirement original intent differs or has duplicate membership");
  return CCLCLifecycleTargetRetirementResultDTO{
      RequestReplayStatusDTO::kReplayedExact, intent.scope, record.retired_targets, intent.request_context};
}

std::optional<CCLCLifecycleTargetRetirementResultDTO>
SqliteCCLCJournal::Impl::ReadLifecycleTargetRetirementResult(
    const CCLCLifecycleTargetRetirementIntentDTO& intent) {
  Require(intent.locally_valid(), "component_lifecycle_journal_query_invalid", "Close result intent is invalid");
  const auto digest = detail::CCLCTargetRetirementSemanticIntentDigest(intent);
  std::lock_guard lock(mutex_);
  RequireOpen();
  ReadTransaction transaction(database_);
  auto result = FindTargetRetirementResultLocked(intent, digest);
  if (!result && UnstartedInFullNamespace(intent.request_context))
    Fail("request_ledger_full", "original Close request was absent from its full namespace");
  transaction.Commit();
  return result;
}

CCLCLifecycleTargetRetirementResultDTO SqliteCCLCJournal::Impl::RetireLifecycleTargetsForClose(
    const RequestOwnerEpoch& owner, const CCLCLifecycleTargetRetirementIntentDTO& intent,
    const ICellCloseAdmissionLease& close_lease) {
  Require(intent.locally_valid() && !owner.empty() && owner.value().size() <= kMaxLifecycleIdentityBytes,
          "component_lifecycle_journal_record_invalid", "Close target retirement intent is invalid");
  const auto digest = detail::CCLCTargetRetirementSemanticIntentDigest(intent);
  const auto blob = detail::EncodeCCLCTargetRetirementRecord({intent, intent.expected_targets});
  // Prepare the complete return value before any durable writes. Positive
  // COMMIT must not turn into an unknown outcome while allocating its echo.
  CCLCLifecycleTargetRetirementResultDTO accepted{
      RequestReplayStatusDTO::kAcceptedNew, intent.scope, intent.expected_targets, intent.request_context};
  TargetRetirementCommitUnknown commit_unknown{{
      "persistence_outcome_unknown", "Close target retirement commit cannot be proved",
      {{"reason", "close_target_retirement_commit_unknown"},
       {"owner_epoch", intent.request_context.request_owner_epoch.value()},
       {"request_id", intent.request_context.request_id.value()}}}};
  auto expected_retired = intent.expected_targets;
  for (auto& target : expected_retired) target.reached = false;
  std::lock_guard lock(mutex_);
  RequireOpen();
  Transaction transaction(database_);
  // The original membership is authoritative before current owner, targets,
  // publication or Kernel prerequisites. A lost reply cannot retire successors.
  if (auto original = FindTargetRetirementResultLocked(intent, digest)) {
    transaction.Commit();
    return std::move(*original);
  }
  Require(owner == intent.scope.request_owner_epoch && ActiveOwner(owner),
          "stale_request_owner_epoch", "fresh Close retirement requires its current owner");
  Require(!MembershipFull(owner), "request_ledger_full", "Close target request namespace is full");
  Require(LoadLifecycleTargetsLocked() == intent.expected_targets,
          "component_lifecycle_target_retirement_conflict", "complete actual lifecycle targets changed");
  const auto before = close_lease.ValidatePrerequisites();
  if (!before.ok()) throw CellClosePrerequisiteFailure(before.error());
  CompactTerminalResults(1U, blob.size());
  Statement insert(database_,
      "INSERT INTO cclc_request_memberships(owner_epoch,request_id,request_kind,semantic_request_digest,"
      "retained_result,target_retirement_blob,target_retirement_sequence) "
      "VALUES(?,?,'lifecycle_target_retirement',?,1,?,?)");
  insert.Bind(1, owner.value());
  insert.Bind(2, intent.request_context.request_id.value());
  insert.Bind(3, digest);
  insert.BindBlob(4, blob);
  const auto terminal_sequence = MintTerminalSequence();
  insert.Bind(5, static_cast<std::int64_t>(terminal_sequence));
  Require(!insert.Step() && sqlite3_changes(database_) == 1,
          "component_lifecycle_journal_corrupt", "Close membership result was not inserted once");
  for (const auto& target : intent.expected_targets)
    RetireLifecycleTargetIntentLocked(target.controllable_component_id,
                                     LifecycleTargetRetirementReasonDTO::kClose);
  CompactTerminalResults();
  // The admission lease remains held, while Stop recognition may still
  // advance real faults. This check can roll back these purely journal writes;
  // it does not promise a future fault-free Close sequence.
  const auto final = close_lease.ValidatePrerequisites();
  if (!final.ok()) throw CellClosePrerequisiteFailure(final.error());
  try {
    transaction.Commit();
  } catch (const JournalFailure&) {
    // An active SQLite transaction has not committed and rolls back in its
    // existing destructor. No unknown classification for that ordinary error.
    if (sqlite3_get_autocommit(database_) == 0) throw;
    bool committed = false;
    bool absent = false;
    try {
      ReadTransaction snapshot(database_);
      Statement membership(database_,
          "SELECT request_kind,semantic_request_digest,retained_result,target_retirement_blob,"
          "target_retirement_sequence FROM cclc_request_memberships WHERE owner_epoch=? AND request_id=?");
      membership.Bind(1, owner.value());
      membership.Bind(2, intent.request_context.request_id.value());
      const bool found = membership.Step();
      committed = found && membership.Text(0) == "lifecycle_target_retirement" &&
          membership.Text(1) == digest && membership.Integer(2) == 1 &&
          !membership.IsNull(3) && membership.Blob(3) == blob &&
          membership.Integer(4) == static_cast<std::int64_t>(terminal_sequence);
      absent = !found;
      if (found && membership.Step()) committed = absent = false;
      const auto remaining = LoadLifecycleTargetsLocked();
      committed = committed && remaining.empty() && ActiveOwner(owner);
      absent = absent && remaining == intent.expected_targets && ActiveOwner(owner);
      for (std::size_t index = 0; index < intent.expected_targets.size(); ++index) {
        const auto actual = ReadLifecycleTargetRetentionLocked(
            intent.expected_targets[index].controllable_component_id);
        committed = committed && actual && !actual->present &&
            actual->retirement_reason == LifecycleTargetRetirementReasonDTO::kClose &&
            actual->target == expected_retired[index];
        absent = absent && actual && actual->present &&
            actual->retirement_reason == LifecycleTargetRetirementReasonDTO::kNone &&
            actual->target == intent.expected_targets[index];
      }
      snapshot.Commit();
    } catch (...) {
      throw std::move(commit_unknown);
    }
    if (committed) return accepted;
    if (absent) throw;
    throw std::move(commit_unknown);
  }
  return accepted;
}

std::vector<ComponentLifecycleTargetDTO> SqliteCCLCJournal::Impl::RetireLifecycleTargetsForClose(
    const RequestOwnerEpoch& owner) {
  Require(!owner.empty() && owner.value().size() <= kMaxLifecycleIdentityBytes,
          "component_lifecycle_journal_query_invalid", "Close owner identity is invalid");
  std::lock_guard lock(mutex_);
  RequireOpen();
  Transaction transaction(database_);
  Require(ActiveOwner(owner), "component_lifecycle_journal_owner_epoch_stale",
          "Close target retirement requires the actual active owner");
  auto targets = LoadLifecycleTargetsLocked();
  for (const auto& target : targets)
    RetireLifecycleTargetIntentLocked(target.controllable_component_id,
                                     LifecycleTargetRetirementReasonDTO::kClose);
  transaction.Commit();
  return targets;
}

std::vector<ComponentLifecycleTargetDTO> SqliteCCLCJournal::Impl::LoadLifecycleTargetsLocked() {
  Statement rows(database_,
                 "SELECT controllable_component_id,requested_state,origin,target_generation,"
                 "reached,recovered_cause FROM cclc_component_lifecycle_targets WHERE present=1 "
                 "ORDER BY controllable_component_id");
  std::vector<ComponentLifecycleTargetDTO> result;
  while (rows.Step()) {
    ComponentLifecycleTargetDTO target{
        ControllableComponentId(rows.Text(0)),
        static_cast<LifecycleTargetStateDTO>(rows.Integer(1)),
        static_cast<LifecycleTargetOriginDTO>(rows.Integer(2)),
        Positive(rows.Integer(3), "lifecycle target generation"),
        rows.Integer(4) == 1,
        rows.IsNull(5)
            ? std::nullopt
            : std::optional(static_cast<LifecycleBlockingCauseClassDTO>(rows.Integer(5)))};
    Require(target.locally_valid() && result.size() < kMaximumCCLCComponentWatermarks,
            "component_lifecycle_journal_corrupt", "lifecycle target rows are invalid");
    result.push_back(std::move(target));
  }
  return result;
}

std::optional<ComponentLifecycleTargetDTO> SqliteCCLCJournal::Impl::LoadLifecycleTarget(
    const ControllableComponentId& component) {
  std::lock_guard lock(mutex_);
  RequireOpen();
  const auto row = ReadLifecycleTargetLocked(component);
  if (!row || !row->second) return std::nullopt;
  return row->first;
}

bool SqliteCCLCJournal::Impl::RecordInterventionCause(
    const ControllableComponentId& component, const ControlDomainId& domain,
    const std::optional<LifecycleBlockingCauseClassDTO> cause_class,
    const std::int64_t observed_at_ns, const std::uint64_t recovery_generation) {
  Require(!component.empty() && component.value().size() <= kMaxLifecycleIdentityBytes &&
              !domain.empty() && domain.value().size() <= kMaxLifecycleIdentityBytes &&
              observed_at_ns >= 0 &&
              (!cause_class || lifecycle_cause_requires_intervention(*cause_class)),
          "component_lifecycle_target_invalid", "intervention cause is invalid");
  std::lock_guard lock(mutex_);
  RequireOpen();
  // Checked again inside the write transaction below; this read keeps an
  // unchanged cause from opening one while still refusing a stale caller.
  RequireRecoveryGenerationLocked(component, recovery_generation);
  std::optional<LifecycleBlockingCauseClassDTO> stored;
  {
    Statement current(database_,
                      "SELECT cause_class FROM cclc_component_intervention_causes "
                      "WHERE controllable_component_id=? AND control_domain_id=?");
    current.Bind(1, component.value());
    current.Bind(2, domain.value());
    if (current.Step()) stored = static_cast<LifecycleBlockingCauseClassDTO>(current.Integer(0));
  }
  // An unchanged cause opens no write transaction: following a target
  // publishes nothing while nothing changes.
  if (stored == cause_class) return false;
  Transaction transaction(database_);
  RequireRecoveryGenerationLocked(component, recovery_generation);
  if (cause_class) {
    Statement write(database_,
                    "INSERT INTO cclc_component_intervention_causes("
                    "controllable_component_id,control_domain_id,cause_class,observed_at_ns) "
                    "VALUES(?,?,?,?) ON CONFLICT(controllable_component_id,control_domain_id) "
                    "DO UPDATE SET cause_class=excluded.cause_class,"
                    "observed_at_ns=excluded.observed_at_ns");
    write.Bind(1, component.value());
    write.Bind(2, domain.value());
    write.Bind(3, static_cast<std::int64_t>(*cause_class));
    write.Bind(4, observed_at_ns);
    Require(!write.Step(), "component_lifecycle_journal_corrupt",
            "intervention cause write returned a row");
  } else {
    Statement clear(database_,
                    "DELETE FROM cclc_component_intervention_causes "
                    "WHERE controllable_component_id=? AND control_domain_id=?");
    clear.Bind(1, component.value());
    clear.Bind(2, domain.value());
    Require(!clear.Step(), "component_lifecycle_journal_corrupt",
            "intervention cause clear returned a row");
  }
  transaction.Commit();
  return true;
}

ComponentLifecycleTargetFactsDTO SqliteCCLCJournal::Impl::LoadLifecycleTargetFacts(
    const ControllableComponentId& component) {
  std::lock_guard lock(mutex_);
  RequireOpen();
  // One snapshot: the generation names exactly the facts returned with it.
  ReadTransaction transaction(database_);
  ComponentLifecycleTargetFactsDTO facts;
  if (const auto row = ReadLifecycleTargetLocked(component); row && row->second) {
    facts.target = row->first;
  }
  facts.recovery_generation = ReadRecoveryGenerationLocked(component);
  facts.intervention_causes = ReadInterventionCausesLocked(component);
  facts.open_incident = ReadOpenComponentIncidentLocked(component, false);
  transaction.Commit();
  return facts;
}

std::vector<ComponentLifecycleInterventionCauseDTO>
SqliteCCLCJournal::Impl::LoadInterventionCauses(const ControllableComponentId& component) {
  std::lock_guard lock(mutex_);
  RequireOpen();
  return ReadInterventionCausesLocked(component);
}

std::vector<ComponentLifecycleInterventionCauseDTO>
SqliteCCLCJournal::Impl::ReadInterventionCausesLocked(const ControllableComponentId& component) {
  Statement rows(database_,
                 "SELECT control_domain_id,cause_class,observed_at_ns "
                 "FROM cclc_component_intervention_causes WHERE controllable_component_id=? "
                 "ORDER BY control_domain_id");
  rows.Bind(1, component.value());
  std::vector<ComponentLifecycleInterventionCauseDTO> result;
  while (rows.Step()) {
    ComponentLifecycleInterventionCauseDTO cause{
        component, ControlDomainId(rows.Text(0)),
        static_cast<LifecycleBlockingCauseClassDTO>(rows.Integer(1)), rows.Integer(2)};
    Require(cause.locally_valid() && result.size() < kMaxLifecycleResolvedDomains,
            "component_lifecycle_journal_corrupt", "intervention cause rows are invalid");
    result.push_back(std::move(cause));
  }
  return result;
}

bool SqliteCCLCJournal::Impl::RecordLifecycleTargetReached(
    const ControllableComponentId& component, const bool reached,
    const std::uint64_t recovery_generation,
    const ComponentLifecycleTargetExpectationDTO* expected_target) {
  std::lock_guard lock(mutex_);
  RequireOpen();
  std::unique_ptr<Transaction> transaction;
  if (expected_target) {
    Require(expected_target->locally_valid() && expected_target->controllable_component_id == component,
            "component_lifecycle_journal_record_invalid", "reached target expectation is invalid");
    transaction = std::make_unique<Transaction>(database_);
  }
  // A Recover since the caller's read reset the marker: never undo that.
  RequireRecoveryGenerationLocked(component, recovery_generation);
  const auto row = ReadLifecycleTargetLocked(component);
  if (expected_target &&
      (!row || !row->second || row->first.target_generation != expected_target->target_generation)) {
    throw JournalFailure(std::string(kLifecycleFactsStaleErrorCode),
                         "the reached marker's original target was retired or superseded",
                         "target_retired_or_superseded");
  }
  // Only a present target carries the marker; an unchanged one writes nothing.
  if (!row || !row->second || row->first.reached == reached) return false;
  if (!transaction) transaction = std::make_unique<Transaction>(database_);
  RequireRecoveryGenerationLocked(component, recovery_generation);
  // Reaching the target again ends the recovery of the last incident.
  Statement write(database_,
                  "UPDATE cclc_component_lifecycle_targets SET reached=?,"
                  "recovered_cause=CASE WHEN ?=1 THEN NULL ELSE recovered_cause END "
                  "WHERE controllable_component_id=? AND present=1 "
                  "AND (? IS NULL OR target_generation=?)");
  write.Bind(1, static_cast<std::int64_t>(reached ? 1 : 0));
  write.Bind(2, static_cast<std::int64_t>(reached ? 1 : 0));
  write.Bind(3, component.value());
  if (expected_target) {
    write.Bind(4, static_cast<std::int64_t>(expected_target->target_generation));
    write.Bind(5, static_cast<std::int64_t>(expected_target->target_generation));
  } else {
    write.BindNull(4);
    write.BindNull(5);
  }
  Require(!write.Step() && (!expected_target || sqlite3_changes(database_) == 1),
          "component_lifecycle_journal_corrupt", "lifecycle target reached write lost its row");
  transaction->Commit();
  return true;
}

namespace {

constexpr std::size_t kMaximumRetainedComponentIncidents = 200U;
constexpr std::size_t kMaximumRetainedComponentIncidentEvents = 1024U;

[[nodiscard]] std::string EncodeVendorCodes(const std::vector<std::string>& codes) {
  content_identity::CanonicalValue::Array values;
  for (const auto& code : codes) values.emplace_back(code);
  return content_identity::canonical_json(content_identity::CanonicalValue(std::move(values)));
}

[[nodiscard]] std::vector<std::string> DecodeVendorCodes(const std::string& json) {
  const auto value = content_identity::parse_canonical_json(json);
  Require(value.is_array() && value.as_array().size() <= kMaxComponentIncidentVendorCodes,
          "component_lifecycle_journal_corrupt", "incident vendor codes are invalid");
  std::vector<std::string> codes;
  for (const auto& code : value.as_array()) {
    Require(code.is_string(), "component_lifecycle_journal_corrupt",
            "incident vendor code is invalid");
    codes.push_back(code.as_string());
  }
  return codes;
}

}  // namespace

std::optional<ComponentIncidentRecordDTO> SqliteCCLCJournal::Impl::ReadOpenComponentIncidentLocked(
    const ControllableComponentId& component, const bool decode_diagnostic) {
  Statement row(database_,
                "SELECT incident_id,cause_class,vendor_codes_json,detected_at_ns,summary,"
                "containment_state,record_json FROM cclc_component_incidents "
                "WHERE target_kind='component' AND controllable_component_id=? AND acknowledged=0");
  row.Bind(1, component.value());
  if (!row.Step()) return std::nullopt;
  ComponentIncidentRecordDTO record{
      ComponentLifecycleIncidentDTO{row.Text(0),
                                    component,
                                    static_cast<LifecycleBlockingCauseClassDTO>(row.Integer(1)),
                                    DecodeVendorCodes(row.Text(2)),
                                    row.Integer(3),
                                    row.Text(4),
                                    std::nullopt,
                                    {},
                                    static_cast<ComponentIncidentContainmentStateDTO>(row.Integer(5)),
                                    std::nullopt,
                                    std::nullopt,
                                    false,
                                    std::nullopt,
                                    std::nullopt,
                                    std::nullopt},
      row.Text(6)};
  // A requested containment's Stop reference lives in the record payload.
  if (record.incident.containment_state == ComponentIncidentContainmentStateDTO::kRequested) {
    record.incident.containment_state = ComponentIncidentContainmentStateDTO::kFailed;
    Require(record.incident.locally_valid(), "component_lifecycle_journal_corrupt",
            "open incident row is invalid");
    record.incident.containment_state = ComponentIncidentContainmentStateDTO::kRequested;
  } else {
    Require(record.incident.locally_valid(), "component_lifecycle_journal_corrupt",
            "open incident row is invalid");
  }
  Require(!row.Step(), "component_lifecycle_journal_corrupt",
          "component has more than one open incident");
  Require(record.record_json.size() <= kMaxComponentIncidentRecordBytes,
          "component_lifecycle_journal_corrupt", "incident record exceeds its bound");
  record.diagnostic_loaded = decode_diagnostic;
  if (decode_diagnostic) {
    const auto receipt = ComponentIncidentContainmentReceiptFromRecord(
        record.record_json, &record.incident.containment_error);
    if (record.incident.containment_state == ComponentIncidentContainmentStateDTO::kRequested) {
      record.incident.containment = receipt;
      Require(record.incident.locally_valid(), "component_lifecycle_journal_corrupt",
              "stored containment receipt is invalid");
    }
    record.diagnostic =
        ComponentIncidentDiagnosticFromRecord(record.record_json, &record.diagnostic_snapshot);
    record.incident.diagnostic = record.diagnostic;
    record.incident.diagnostic_snapshot = record.diagnostic_snapshot;
  }
  return record;
}

std::string SqliteCCLCJournal::Impl::PreserveComponentIncidentDiagnosticLocked(
    const std::string& incident_id, const std::string& record_json) {
  Statement row(database_,
                "SELECT record_json FROM cclc_component_incidents WHERE target_kind='component' "
                "AND incident_id=?");
  row.Bind(1, incident_id);
  if (row.Step()) {
    std::shared_ptr<const content_identity::CanonicalValue> snapshot;
    const auto diagnostic = ComponentIncidentDiagnosticFromRecord(row.Text(0), &snapshot);
    if (diagnostic)
      return ComponentIncidentRecordWithDiagnostic(record_json, *diagnostic, snapshot);
  }
  return record_json;
}

void SqliteCCLCJournal::Impl::AppendComponentIncidentEventLocked(
    const std::string_view event_kind, const std::string& incident_id,
    const std::int64_t occurred_at_ns, const std::string& payload_json) {
  const auto next = ScalarInteger(
      database_, "SELECT coalesce(max(event_sequence),0)+1 FROM cclc_component_incident_events");
  Statement write(database_,
                  "INSERT INTO cclc_component_incident_events("
                  "event_sequence,incident_id,event_kind,occurred_at_ns,payload_json) "
                  "VALUES(?,?,?,?,?)");
  write.Bind(1, next);
  write.Bind(2, incident_id);
  write.Bind(3, event_kind);
  write.Bind(4, occurred_at_ns);
  write.Bind(5, payload_json);
  Require(!write.Step(), "component_lifecycle_journal_corrupt",
          "incident event write returned a row");
  // Bounded history: the oldest events are dropped; sequences never repeat
  // because the newest row always survives.
  Statement trim(database_,
                 "DELETE FROM cclc_component_incident_events WHERE event_sequence <= ?");
  trim.Bind(1, next - static_cast<std::int64_t>(kMaximumRetainedComponentIncidentEvents));
  Require(!trim.Step(), "component_lifecycle_journal_corrupt",
          "incident event trim returned a row");
}

bool SqliteCCLCJournal::Impl::ComponentInUnadoptedApplyPlan(
    const ControllableComponentId& component) {
  RefreshNativeApplyPreparationFailure();
  std::lock_guard lock(mutex_);
  RequireOpen();
  if (!ReadUnsettledNativeApplyPlanLocked()) return false;
  // Only a component the plan installs, replaces or removes: one retained
  // unchanged keeps its reached status and its ordinary incident rules.
  Statement named(database_,
                  "SELECT 1 FROM cclc_native_apply_plan_components "
                  "WHERE controllable_component_id=?1 AND retained=0 UNION ALL "
                  "SELECT 1 FROM cclc_native_apply_plan_removals "
                  "WHERE removal_kind='component' AND identity=?1");
  named.Bind(1, component.value());
  return named.Step();
}

std::optional<CellObserverIncidentRecordDTO>
SqliteCCLCJournal::Impl::ReadOpenCellObserverIncidentLocked(
    CellObserverLossAssociationDTO* const association) {
  Statement row(database_,
                "SELECT incident_id,observer_cell_id,cause_class,vendor_codes_json,"
                "detected_at_ns,summary,record_json,observer_latest_exit_id,"
                "observer_latest_exit_digest FROM cclc_component_incidents "
                "WHERE target_kind='cell_observer' AND acknowledged=0");
  if (!row.Step()) return std::nullopt;
  const auto record = [&] {
    try {
      return DecodeCellObserverIncidentRecord(row.Text(6));
    } catch (const std::exception&) {
      Fail("component_lifecycle_journal_corrupt", "stored cell observer incident record is invalid");
    }
  }();
  Require(record.incident_id == row.Text(0) && record.target.cell_id == cell_id_ &&
              row.Text(1) == cell_id_.value() &&
              row.Integer(2) ==
                  static_cast<std::int64_t>(LifecycleBlockingCauseClassDTO::kProviderLost) &&
              row.Text(3) == "[]" && record.detected_at_ns == row.Integer(4) &&
              record.summary == row.Text(5) && !row.Text(7).empty() &&
              row.Text(7).size() <= kMaxComponentIncidentIdBytes &&
              IsExactLowercaseSha256Digest(std::string(kSha256DigestPrefix) + row.Text(8)),
          "component_lifecycle_journal_corrupt", "cell observer incident SQL and payload disagree");
  if (association)
    *association = {row.Text(0), row.Text(7), row.Text(8)};
  Require(!row.Step(), "component_lifecycle_journal_corrupt",
          "cell observer slot has more than one open incident");
  return record;
}

std::optional<CellObserverIncidentRecordDTO>
SqliteCCLCJournal::Impl::LoadOpenCellObserverIncident() {
  std::lock_guard lock(mutex_);
  RequireOpen();
  return ReadOpenCellObserverIncidentLocked();
}

std::optional<CellObserverLossAssociationDTO>
SqliteCCLCJournal::Impl::LoadOpenCellObserverLossAssociation() {
  std::lock_guard lock(mutex_);
  RequireOpen();
  ReadTransaction transaction(database_);
  CellObserverLossAssociationDTO association;
  const auto incident = ReadOpenCellObserverIncidentLocked(&association);
  transaction.Commit();
  return incident ? std::optional<CellObserverLossAssociationDTO>(std::move(association))
                  : std::nullopt;
}

CellObserverIncidentRecordDTO SqliteCCLCJournal::Impl::RecordCellObserverIncident(
    const CellObserverIncidentRecordDTO& record) {
  Require(record.locally_valid() && record.target.cell_id == cell_id_,
          "component_lifecycle_incident_invalid",
          "observer exit does not name this real cell slot");
  const auto encode = [](const CellObserverIncidentRecordDTO& value) {
    try {
      return EncodeCellObserverIncidentRecord(value);
    } catch (const std::invalid_argument& error) {
      Fail("component_lifecycle_incident_invalid", error.what());
    }
  };
  // The actual input is qualified before exclusion. The final event envelope
  // depends on the current open slot and is qualified before its first write.
  const auto input_bytes = encode(record);
  // The storage field is the approved 64 hexadecimal bytes; the hashing
  // API returns the portable "sha256:" prefix as well.
  const auto input_digest =
      content_identity::sha256_digest(input_bytes).substr(kSha256DigestPrefix.size());
  std::unique_lock lock(mutex_);
  RequireOpen();
  Transaction transaction(database_);
  const auto open = ReadOpenCellObserverIncidentLocked();
  auto response = open ? *open : record;
  static_assert(std::is_nothrow_move_constructible_v<CellObserverIncidentRecordDTO>);
  auto event_record = record;
  if (open) {
    event_record.incident_id = open->incident_id;
    Statement latest(database_,
                     "SELECT observer_latest_exit_id,observer_latest_exit_digest "
                     "FROM cclc_component_incidents WHERE target_kind='cell_observer' "
                     "AND incident_id=? AND acknowledged=0");
    latest.Bind(1, open->incident_id);
    Require(latest.Step(), "component_lifecycle_journal_corrupt", "observer open row disappeared");
    if (latest.Text(0) == record.incident_id) {
      Require(latest.Text(1) == input_digest, "component_lifecycle_incident_conflict",
              "same current observer exit identity has different evidence");
      return response;
    }
  }
  const auto event_bytes = encode(event_record);
  ComponentIncidentPruneNotice pruned;
  if (open) {
    Statement latest(database_,
                     "UPDATE cclc_component_incidents SET observer_latest_exit_id=?,"
                     "observer_latest_exit_digest=? WHERE target_kind='cell_observer' "
                     "AND incident_id=? AND acknowledged=0");
    latest.Bind(1, record.incident_id);
    latest.Bind(2, input_digest);
    latest.Bind(3, open->incident_id);
    Require(!latest.Step() && sqlite3_changes(database_) == 1,
            "component_lifecycle_journal_corrupt", "observer latest exit write lost its open row");
  } else {
    const auto sequence = ScalarInteger(
        database_, "SELECT coalesce(max(incident_sequence),0)+1 FROM cclc_component_incidents");
    Statement write(database_,
                    "INSERT INTO cclc_component_incidents(incident_id,incident_sequence,"
                    "target_kind,observer_cell_id,observer_latest_exit_id,"
                    "observer_latest_exit_digest,cause_class,vendor_codes_json,"
                    "detected_at_ns,summary,record_json,acknowledged) "
                    "VALUES(?,?,'cell_observer',?,?,?,?, '[]',?,?,?,0)");
    write.Bind(1, record.incident_id);
    write.Bind(2, sequence);
    write.Bind(3, cell_id_.value());
    write.Bind(4, record.incident_id);
    write.Bind(5, input_digest);
    write.Bind(6, static_cast<std::int64_t>(LifecycleBlockingCauseClassDTO::kProviderLost));
    write.Bind(7, record.detected_at_ns);
    write.Bind(8, record.summary);
    write.Bind(9, input_bytes);
    Require(!write.Step(), "component_lifecycle_journal_corrupt", "observer insert returned a row");
    pruned = PruneClosedIncidentsLocked();
  }
  AppendComponentIncidentEventLocked(kComponentIncidentEventKind, event_record.incident_id,
                                     record.detected_at_ns, event_bytes);
  transaction.Commit();
  lock.unlock();
  NotifyIncidentPruning(pruned);
  return response;
}

namespace {
std::string ObserverBringUpBytes(const CellObserverBringUpRecordDTO& record) {
  try {
    return EncodeCellObserverBringUpRecord(record);
  } catch (const std::length_error&) {
    Fail("observer_bring_up_record_too_large",
         "observer bring-up exceeds the 512 KiB encoded bound");
  } catch (const std::exception& error) {
    Fail("observer_bring_up_invalid", error.what());
  }
}
bool SameObserverRequest(const PublicRequestContextDTO& left,
                         const PublicRequestContextDTO& right) {
  return left.request_id == right.request_id &&
         left.request_owner_epoch == right.request_owner_epoch;
}
bool ObserverUpdatePhase(CellObserverBringUpPhaseDTO before, CellObserverBringUpPhaseDTO after) {
  using Phase = CellObserverBringUpPhaseDTO;
  if (before == Phase::kAdmitted)
    return after == Phase::kRetiring || after == Phase::kFailed || after == Phase::kUnknown;
  if (before == Phase::kRetiring)
    return after == Phase::kStarted || after == Phase::kFailed || after == Phase::kUnknown;
  if (before == Phase::kStarted) return after == Phase::kFailed || after == Phase::kUnknown;
  return false;
}
}  // namespace

std::optional<CellObserverBringUpRecordDTO>
SqliteCCLCJournal::Impl::ReadLatestCellObserverBringUpLocked() {
  Statement row(database_,
                "SELECT observer_latest_bring_up FROM cclc_journal_metadata WHERE singleton=1");
  Require(row.Step(), "component_lifecycle_journal_corrupt",
          "observer bring-up metadata is missing");
  std::optional<CellObserverBringUpRecordDTO> result;
  if (!row.IsNull(0)) {
    try {
      result = DecodeCellObserverBringUpRecord(row.Blob(0));
    } catch (const std::exception&) {
      Fail("component_lifecycle_journal_corrupt", "retained observer bring-up record is invalid");
    }
    Require(result->intent.replacement.cell_id == cell_id_, "component_lifecycle_journal_corrupt",
            "retained observer bring-up names a foreign cell");
  }
  Require(!row.Step(), "component_lifecycle_journal_corrupt",
          "observer bring-up metadata is duplicated");
  return result;
}
void SqliteCCLCJournal::Impl::WriteLatestCellObserverBringUpLocked(const std::string& bytes) {
  Statement write(database_,
                  "UPDATE cclc_journal_metadata SET observer_latest_bring_up=? WHERE singleton=1");
  write.BindBlob(1, bytes);
  Require(!write.Step() && sqlite3_changes(database_) == 1, "component_lifecycle_journal_corrupt",
          "observer bring-up metadata write lost its row");
}
std::optional<CellObserverBringUpRecordDTO>
SqliteCCLCJournal::Impl::LoadLatestCellObserverBringUp() {
  std::lock_guard lock(mutex_);
  RequireOpen();
  return ReadLatestCellObserverBringUpLocked();
}
CellObserverBringUpAdmissionDTO SqliteCCLCJournal::Impl::BeginLatestCellObserverBringUp(
    const CellObserverBringUpRecordDTO& admitted,
    const std::optional<CellObserverBringUpRecordDTO>& expected_latest) {
  static_assert(std::is_nothrow_move_constructible_v<CellObserverBringUpAdmissionDTO>);
  Require(admitted.locally_valid() && admitted.intent.replacement.cell_id == cell_id_ &&
              admitted.phase == CellObserverBringUpPhaseDTO::kAdmitted &&
              admitted.admitted_at_ns == admitted.updated_at_ns,
          "observer_bring_up_invalid",
          "observer bring-up admission does not name this cell or initial outcome");
  std::lock_guard lock(mutex_);
  RequireOpen();
  // Exclude other journal connections before comparing the durable latest
  // attempt. The instance mutex alone cannot establish this CAS.
  Transaction transaction(database_);
  const auto current = ReadLatestCellObserverBringUpLocked();
  if (current &&
      SameObserverRequest(current->intent.request_context, admitted.intent.request_context)) {
    auto replay_intent = admitted.intent;
    // Ordinary request replay excludes transport-derived attribution. Return
    // the original durable provenance and admission evidence unchanged.
    replay_intent.attribution = current->intent.attribution;
    Require(current->intent == replay_intent, "observer_bring_up_request_conflict",
            "same observer request has different semantic intent");
    return {*current, false};
  }
  Require(current == expected_latest, "observer_bring_up_stale",
          "latest observer attempt changed before admission");
  CellObserverLossAssociationDTO association;
  const auto incident = ReadOpenCellObserverIncidentLocked(&association);
  const auto incident_id =
      incident ? std::optional<std::string>(incident->incident_id) : std::nullopt;
  Require(incident_id == admitted.intent.predecessor_incident_id,
          "observer_bring_up_incident_changed",
          "observer incident changed before explicit admission");
  const auto loss = incident
      ? std::optional<CellObserverLossAssociationDTO>(std::move(association)) : std::nullopt;
  Require(loss == admitted.predecessor_loss, "observer_bring_up_loss_changed",
          "observer loss changed before explicit admission");
  const auto bytes = ObserverBringUpBytes(admitted);
  CellObserverBringUpAdmissionDTO response{admitted, true};
  WriteLatestCellObserverBringUpLocked(bytes);
  transaction.Commit();
  return response;
}
CellObserverBringUpRecordDTO SqliteCCLCJournal::Impl::UpdateLatestCellObserverBringUp(
    const CellObserverBringUpRecordDTO& expected, const CellObserverBringUpRecordDTO& updated) {
  static_assert(std::is_nothrow_move_constructible_v<CellObserverBringUpRecordDTO>);
  const auto bytes = ObserverBringUpBytes(updated);
  Require(expected.locally_valid() && updated.intent == expected.intent &&
              updated.intent.replacement.cell_id == cell_id_ &&
              updated.admitted_at_ns == expected.admitted_at_ns &&
              updated.updated_at_ns >= expected.updated_at_ns &&
              updated.predecessor_process == expected.predecessor_process &&
              updated.predecessor_loss == expected.predecessor_loss &&
              (!expected.predecessor_drained_or_absent || updated.predecessor_drained_or_absent) &&
              (!expected.started_process || updated.started_process == expected.started_process) &&
              !updated.first_open && ObserverUpdatePhase(expected.phase, updated.phase),
          "observer_bring_up_invalid",
          "observer update changes original identity or is not an actual outcome transition");
  auto response = updated;
  std::lock_guard lock(mutex_);
  RequireOpen();
  Transaction transaction(database_);
  const auto current = ReadLatestCellObserverBringUpLocked();
  if (current && *current == updated) return response;
  Require(current && *current == expected, "observer_bring_up_stale",
          "latest observer attempt changed before update");
  WriteLatestCellObserverBringUpLocked(bytes);
  transaction.Commit();
  return response;
}
CellObserverBringUpRecordDTO SqliteCCLCJournal::Impl::CloseCellObserverIncidentByOperatorBringUp(
    const CellObserverBringUpRecordDTO& expected_started,
    const CellObserverBringUpOpenDTO& first_open) {
  Require(expected_started.phase == CellObserverBringUpPhaseDTO::kStarted &&
              expected_started.locally_valid() && first_open.locally_valid() &&
              expected_started.intent.replacement.cell_id == cell_id_,
          "observer_bring_up_invalid",
          "observer closure needs the exact started attempt and first accepted Open");
  auto completed = expected_started;
  completed.phase = CellObserverBringUpPhaseDTO::kOpenAccepted;
  completed.first_open = first_open;
  completed.updated_at_ns = std::max(completed.updated_at_ns, first_open.accepted_at_ns);
  const auto bytes = ObserverBringUpBytes(completed);
  static_assert(std::is_nothrow_move_constructible_v<CellObserverBringUpRecordDTO>);
  std::unique_lock lock(mutex_);
  RequireOpen();
  Transaction transaction(database_);
  const auto current = ReadLatestCellObserverBringUpLocked();
  if (current && *current == completed) return completed;
  Require(current && *current == expected_started, "observer_bring_up_stale",
          "first accepted Open does not name the latest exact started attempt");
  CellObserverLossAssociationDTO association;
  const auto incident = ReadOpenCellObserverIncidentLocked(&association);
  const auto& original_id = expected_started.intent.predecessor_incident_id;
  const auto incident_id =
      incident ? std::optional<std::string>(incident->incident_id) : std::nullopt;
  Require(incident_id == original_id, "observer_bring_up_incident_changed",
          "observer incident changed since explicit bring-up admission");
  const auto loss = incident
      ? std::optional<CellObserverLossAssociationDTO>(std::move(association)) : std::nullopt;
  Require(loss == expected_started.predecessor_loss, "observer_bring_up_loss_changed",
          "observer loss changed after explicit bring-up admission");
  std::string event;
  if (incident) {
    auto root = content_identity::parse_canonical_json(EncodeCellObserverIncidentRecord(*incident));
    auto fields = root.as_object();
    for (auto& [key, value] : fields) {
      if (key == "event_kind")
        value =
            content_identity::CanonicalValue(std::string(kComponentIncidentAcknowledgedEventKind));
      if (key == "acknowledgement")
        value = content_identity::CanonicalValue(content_identity::CanonicalValue::Object{
            {"request_id", content_identity::CanonicalValue(
                               expected_started.intent.request_context.request_id.value())},
            {"request_owner_epoch",
             content_identity::CanonicalValue(
                 expected_started.intent.request_context.request_owner_epoch.value())},
            {"source_label", content_identity::CanonicalValue(
                                 expected_started.intent.request_context.source_label)},
            {"at_ns", content_identity::nonnegative_int64_value(first_open.accepted_at_ns)},
            {"reason",
             content_identity::CanonicalValue(std::string("superseded by operator bring-up"))},
            {"first_open",
             content_identity::CanonicalValue(content_identity::CanonicalValue::Object{
                 {"source_epoch", content_identity::CanonicalValue(first_open.source_epoch)},
                 {"source_epoch_order", content_identity::CanonicalValue(static_cast<double>(
                                            first_open.source_epoch_order))}})}});
    }
    event = content_identity::canonical_json(content_identity::CanonicalValue(std::move(fields)));
    Require(event.size() <= kMaxComponentIncidentRecordBytes, "observer_bring_up_record_too_large",
            "observer acknowledgement exceeds the 512 KiB encoded bound");
  }
  WriteLatestCellObserverBringUpLocked(bytes);
  if (incident) {
    Statement close(
        database_,
        "UPDATE cclc_component_incidents SET acknowledged=1,acknowledged_by=?,"
        "acknowledged_at_ns=?,acknowledgement_request_id=?,acknowledgement_owner_epoch=? "
        "WHERE target_kind='cell_observer' AND incident_id=? AND acknowledged=0");
    close.Bind(1, expected_started.intent.request_context.source_label);
    close.Bind(2, first_open.accepted_at_ns);
    close.Bind(3, expected_started.intent.request_context.request_id.value());
    close.Bind(4, expected_started.intent.request_context.request_owner_epoch.value());
    close.Bind(5, incident->incident_id);
    Require(!close.Step() && sqlite3_changes(database_) == 1, "component_lifecycle_journal_corrupt",
            "observer acknowledgement lost its exact incident");
    AppendComponentIncidentEventLocked(kComponentIncidentAcknowledgedEventKind,
                                       incident->incident_id, first_open.accepted_at_ns, event);
  }
  const auto pruned = PruneClosedIncidentsLocked();
  transaction.Commit();
  lock.unlock();
  NotifyIncidentPruning(pruned);
  return completed;
}

ComponentIncidentPruneNotice SqliteCCLCJournal::Impl::PruneClosedIncidentsLocked() {
  // Closed rows have their own count bound. Open incidents never consume it
  // and can never be removed by this cleanup.
  ComponentIncidentPruneNotice pruned;
  Statement cutoff(database_,
                   "SELECT incident_sequence FROM cclc_component_incidents "
                   "WHERE acknowledged=1 ORDER BY incident_sequence DESC LIMIT 1 OFFSET ?");
  cutoff.Bind(1, static_cast<std::int64_t>(kMaximumRetainedComponentIncidents));
  if (cutoff.Step()) {
    const auto retained_floor = cutoff.Integer(0);
    Statement notice(database_,
                     "SELECT count(*),min(incident_sequence),max(incident_sequence) "
                     "FROM cclc_component_incidents WHERE acknowledged=1 AND incident_sequence<=?");
    notice.Bind(1, retained_floor);
    Require(notice.Step(), "component_lifecycle_journal_corrupt",
            "incident prune count is unavailable");
    pruned = {static_cast<std::uint64_t>(notice.Integer(0)),
              static_cast<std::uint64_t>(notice.Integer(1)),
              static_cast<std::uint64_t>(notice.Integer(2))};
    Statement expire(database_,
                     "UPDATE cclc_request_memberships SET retained_result=0 WHERE EXISTS ("
                     "SELECT 1 FROM cclc_component_incidents i WHERE i.target_kind='component' AND "
                     "i.acknowledged=1 AND "
                     "i.incident_sequence <= ? AND "
                     "i.acknowledgement_owner_epoch=cclc_request_memberships.owner_epoch AND "
                     "i.acknowledgement_request_id=cclc_request_memberships.request_id)");
    expire.Bind(1, retained_floor);
    Require(!expire.Step(), "component_lifecycle_journal_corrupt",
            "incident recovery expiry returned a row");
    Statement trim(
        database_,
        "DELETE FROM cclc_component_incidents WHERE acknowledged=1 AND incident_sequence<=?");
    trim.Bind(1, retained_floor);
    Require(!trim.Step(), "component_lifecycle_journal_corrupt", "incident trim returned a row");
    Require(static_cast<std::uint64_t>(sqlite3_changes(database_)) == pruned.count,
            "component_lifecycle_journal_corrupt",
            "incident prune count differs from removed rows");
  }
  return pruned;
}

void SqliteCCLCJournal::Impl::NotifyIncidentPruning(
    const ComponentIncidentPruneNotice& notice) noexcept {
  if (notice.count && on_incidents_pruned_) {
    // The host callback queues one bounded INFO notice. Failure is auxiliary:
    // the committed pruning and successful request remain successful.
    try {
      on_incidents_pruned_(notice);
    } catch (...) {
    }
  }
}

ComponentIncidentRecordDTO SqliteCCLCJournal::Impl::OpenComponentIncident(
    const ComponentIncidentRecordDTO& record, const std::uint64_t recovery_generation) {
  const auto& incident = record.incident;
  Require(incident.locally_valid() && !incident.acknowledged &&
              incident.containment_state == ComponentIncidentContainmentStateDTO::kPending &&
              !record.record_json.empty() &&
              record.record_json.size() <= kMaxComponentIncidentRecordBytes,
          "component_lifecycle_incident_invalid", "component incident is invalid");
  std::unique_lock lock(mutex_);
  RequireOpen();
  // A detection made from facts read before the operator's Recover is not
  // recorded: the driver re-evaluates on facts that include the Recover.
  RequireRecoveryGenerationLocked(incident.controllable_component_id, recovery_generation);
  if (auto existing = ReadOpenComponentIncidentLocked(incident.controllable_component_id)) {
    return std::move(*existing);
  }
  Transaction transaction(database_);
  RequireRecoveryGenerationLocked(incident.controllable_component_id, recovery_generation);
  const auto sequence = ScalarInteger(
      database_, "SELECT coalesce(max(incident_sequence),0)+1 FROM cclc_component_incidents");
  Statement write(database_,
                  "INSERT INTO cclc_component_incidents("
                  "incident_id,incident_sequence,controllable_component_id,cause_class,"
                  "vendor_codes_json,detected_at_ns,summary,containment_state,record_json,"
                  "acknowledged) VALUES(?,?,?,?,?,?,?,0,?,0)");
  write.Bind(1, incident.incident_id);
  write.Bind(2, sequence);
  write.Bind(3, incident.controllable_component_id.value());
  write.Bind(4, static_cast<std::int64_t>(incident.cause_class));
  write.Bind(5, EncodeVendorCodes(incident.vendor_codes));
  write.Bind(6, incident.detected_at_ns);
  write.Bind(7, incident.summary);
  write.Bind(8, record.record_json);
  Require(!write.Step(), "component_lifecycle_journal_corrupt", "incident write returned a row");
  const auto target = ReadLifecycleTargetLocked(incident.controllable_component_id);
  if (target && target->second && target->first.requested_state == LifecycleTargetStateDTO::kActive)
    RetireLifecycleTargetIntentLocked(incident.controllable_component_id);
  const auto pruned = PruneClosedIncidentsLocked();
  transaction.Commit();
  lock.unlock();
  NotifyIncidentPruning(pruned);
  return record;
}

std::optional<ComponentIncidentRecordDTO> SqliteCCLCJournal::Impl::LoadOpenComponentIncident(
    const ControllableComponentId& component) {
  std::lock_guard lock(mutex_);
  RequireOpen();
  return ReadOpenComponentIncidentLocked(component);
}

bool SqliteCCLCJournal::Impl::RecordComponentIncidentContainment(
    const std::string& incident_id, const ComponentIncidentContainmentStateDTO state,
    const std::string& record_json, const std::int64_t occurred_at_ns) {
  Require(!incident_id.empty() && incident_id.size() <= kMaxComponentIncidentIdBytes &&
              state != ComponentIncidentContainmentStateDTO::kPending && !record_json.empty() &&
              record_json.size() <= kMaxComponentIncidentRecordBytes && occurred_at_ns >= 0,
          "component_lifecycle_incident_invalid", "incident containment record is invalid");
  std::lock_guard lock(mutex_);
  RequireOpen();
  Transaction transaction(database_);
  const auto preserved = PreserveComponentIncidentDiagnosticLocked(incident_id, record_json);
  Statement write(
      database_,
      "UPDATE cclc_component_incidents SET containment_state=?,record_json=? "
      "WHERE target_kind='component' AND incident_id=? AND acknowledged=0 AND containment_state=0");
  write.Bind(1, static_cast<std::int64_t>(state));
  write.Bind(2, preserved);
  write.Bind(3, incident_id);
  Require(!write.Step(), "component_lifecycle_journal_corrupt",
          "incident containment write returned a row");
  if (sqlite3_changes(database_) != 1) return false;
  AppendComponentIncidentEventLocked(kComponentIncidentEventKind, incident_id, occurred_at_ns,
                                     preserved);
  transaction.Commit();
  return true;
}

std::optional<CCLCLifecycleRequestResultDTO> SqliteCCLCJournal::Impl::RecoverComponentIncident(
    const RequestOwnerEpoch& active_owner_epoch, const ComponentLifecycleIntentDTO& intent,
    const std::string& acknowledged_by, const std::int64_t acknowledged_at_ns) {
  Require(!active_owner_epoch.empty() && intent.locally_valid() &&
              intent.intent == ComponentLifecycleIntentKindDTO::kRecover &&
              intent.request_context.source_label != kLifecycleTargetSourceLabel &&
              !acknowledged_by.empty() && acknowledged_at_ns >= 0,
          "component_lifecycle_incident_invalid", "incident recovery request is invalid");
  const auto semantic_digest = detail::ComponentLifecycleSemanticIntentDigest(intent);
  std::lock_guard lock(mutex_);
  RequireOpen();
  Transaction transaction(database_);
  // The ordinary request identity decides first: an exact replay, a reused
  // request id with other content, a stale owner epoch or a full namespace
  // is answered by ordinary CCLC admission from these same records.
  if (const auto inspected = InspectLifecycleRequestLocked(active_owner_epoch, intent, semantic_digest)) {
    transaction.Commit();
    return intent.close_incident_recovery ? inspected : std::nullopt;
  }
  const auto open = ReadOpenComponentIncidentLocked(intent.controllable_component_id);
  if (intent.close_incident_recovery)
    Require(open && open->incident.incident_id ==
                        intent.close_incident_recovery->expected_incident_id,
            "component_lifecycle_close_incident_mismatch",
            "Close recovery must match the actual open incident");
  if (!open) {
    transaction.Commit();
    return std::nullopt;
  }
  auto incident = open->incident;
  incident.acknowledged = true;
  incident.acknowledged_by = acknowledged_by;
  incident.acknowledged_at_ns = acknowledged_at_ns;
  incident.acknowledgement_request_id = intent.request_context.request_id;
  const auto closure = intent.close_incident_recovery
      ? std::optional{ComponentIncidentClosureReasonDTO::kCellClosedByOperator} : std::nullopt;
  const auto payload = ComponentIncidentAcknowledgedPayload(open->record_json, incident, closure);
  Require(payload.size() <= kMaxComponentIncidentRecordBytes,
          "component_lifecycle_incident_invalid", "incident acknowledgement record is too large");

  // Authorization only: one membership with no transaction; its result is
  // the acknowledged incident row below.
  Statement membership(database_,
                       "INSERT INTO cclc_request_memberships("
                       "owner_epoch,request_id,request_kind,semantic_request_digest,"
                       "retained_result) VALUES(?,?,'component_lifecycle',?,1)");
  membership.Bind(1, active_owner_epoch.value());
  membership.Bind(2, intent.request_context.request_id.value());
  membership.Bind(3, semantic_digest);
  Require(!membership.Step(), "component_lifecycle_journal_corrupt",
          "CCLC request membership insert returned an unexpected row");

  Statement close(database_,
                  "UPDATE cclc_component_incidents SET acknowledged=1,acknowledged_by=?,"
                  "acknowledged_at_ns=?,acknowledgement_request_id=?,"
                  "acknowledgement_owner_epoch=? "
                  "WHERE target_kind='component' AND incident_id=? AND controllable_component_id=? AND acknowledged=0");
  close.Bind(1, acknowledged_by);
  close.Bind(2, acknowledged_at_ns);
  close.Bind(3, intent.request_context.request_id.value());
  close.Bind(4, active_owner_epoch.value());
  close.Bind(5, incident.incident_id);
  close.Bind(6, incident.controllable_component_id.value());
  Require(!close.Step() && sqlite3_changes(database_) == 1,
          "component_lifecycle_journal_corrupt", "incident acknowledgement did not close it");
  // Preserve the typed acknowledgement in the same bounded incident row so
  // exact result replay survives owner restart; no second recovery ledger.
  if (closure) {
    Statement evidence(database_, "UPDATE cclc_component_incidents SET record_json=? "
                                  "WHERE incident_id=? AND acknowledged=1");
    evidence.Bind(1, payload);
    evidence.Bind(2, incident.incident_id);
    Require(!evidence.Step() && sqlite3_changes(database_) == 1,
            "component_lifecycle_journal_corrupt", "Close acknowledgement lost its incident");
  }
  // The acknowledgement clears what fresh status alone would otherwise have
  // to clear, and starts the component over: it must reach its target again
  // before a further loss is a new incident.
  Statement causes(database_,
                   "DELETE FROM cclc_component_intervention_causes "
                   "WHERE controllable_component_id=?");
  causes.Bind(1, incident.controllable_component_id.value());
  Require(!causes.Step(), "component_lifecycle_journal_corrupt",
          "intervention cause removal returned a row");
  Statement reached(database_,
                    "UPDATE cclc_component_lifecycle_targets SET reached=0,recovered_cause=? "
                    "WHERE controllable_component_id=?");
  reached.Bind(1, static_cast<std::int64_t>(incident.cause_class));
  reached.Bind(2, incident.controllable_component_id.value());
  Require(!reached.Step(), "component_lifecycle_journal_corrupt",
          "lifecycle target reached reset returned a row");
  // A driver pass that read the facts before this commit may not reopen,
  // extend or re-retain what this Recover closed and cleared.
  AdvanceRecoveryGenerationLocked(incident.controllable_component_id);
  AppendComponentIncidentEventLocked(kComponentIncidentAcknowledgedEventKind,
                                     incident.incident_id, acknowledged_at_ns, payload);
  transaction.Commit();
  CCLCLifecycleRequestResultDTO result{RequestReplayStatusDTO::kAcceptedNew, std::nullopt,
                                       std::nullopt};
  result.incident_recovery = ComponentIncidentRecoveryDTO{
      incident.incident_id, incident.controllable_component_id, incident.cause_class,
      acknowledged_by, acknowledged_at_ns, closure};
  return result;
}

std::optional<ComponentIncidentRecoveryDTO>
SqliteCCLCJournal::Impl::ReadComponentIncidentRecoveryLocked(
    const PublicRequestContextDTO& request) {
  Statement row(database_,
                "SELECT incident_id,controllable_component_id,cause_class,acknowledged_by,"
                "acknowledged_at_ns,record_json FROM cclc_component_incidents WHERE target_kind='component' "
                "AND acknowledged=1 AND "
                "acknowledgement_owner_epoch=? AND acknowledgement_request_id=?");
  row.Bind(1, request.request_owner_epoch.value());
  row.Bind(2, request.request_id.value());
  if (!row.Step()) return std::nullopt;
  ComponentIncidentRecoveryDTO recovery{row.Text(0), ControllableComponentId(row.Text(1)),
                                        static_cast<LifecycleBlockingCauseClassDTO>(row.Integer(2)),
                                        row.Text(3), row.Integer(4)};
  const auto evidence = content_identity::parse_canonical_json(row.Text(5));
  if (const auto* closure = evidence.find_object_member("closure_reason")) {
    Require(closure->is_string() && closure->as_string() == "cell_closed_by_operator",
            "component_lifecycle_journal_corrupt", "incident closure reason is invalid");
    recovery.closure_reason = ComponentIncidentClosureReasonDTO::kCellClosedByOperator;
  }
  Require(recovery.locally_valid() && !row.Step(), "component_lifecycle_journal_corrupt",
          "acknowledged incident row is invalid");
  return recovery;
}

bool SqliteCCLCJournal::Impl::UpdateOpenComponentIncident(
    const std::string& incident_id, const LifecycleBlockingCauseClassDTO cause_class,
    const std::vector<std::string>& vendor_codes, const std::string& record_json) {
  Require(!incident_id.empty() && incident_id.size() <= kMaxComponentIncidentIdBytes &&
              lifecycle_cause_requires_intervention(cause_class) &&
              vendor_codes.size() <= kMaxComponentIncidentVendorCodes && !record_json.empty() &&
              record_json.size() <= kMaxComponentIncidentRecordBytes,
          "component_lifecycle_incident_invalid", "incident update is invalid");
  std::lock_guard lock(mutex_);
  RequireOpen();
  Transaction transaction(database_);
  const auto preserved = PreserveComponentIncidentDiagnosticLocked(incident_id, record_json);
  Statement write(database_,
                  "UPDATE cclc_component_incidents SET cause_class=?,vendor_codes_json=?,"
                  "record_json=? WHERE target_kind='component' AND incident_id=? AND acknowledged=0");
  write.Bind(1, static_cast<std::int64_t>(cause_class));
  write.Bind(2, EncodeVendorCodes(vendor_codes));
  write.Bind(3, preserved);
  write.Bind(4, incident_id);
  Require(!write.Step(), "component_lifecycle_journal_corrupt",
          "incident update returned a row");
  if (sqlite3_changes(database_) != 1) return false;
  transaction.Commit();
  return true;
}

bool SqliteCCLCJournal::Impl::AppendComponentIncidentDiagnostic(
    const std::string& incident_id, const std::int64_t occurred_at_ns, const ErrorDTO& diagnostic,
    std::shared_ptr<const content_identity::CanonicalValue> snapshot) {
  Require(!incident_id.empty() && incident_id.size() <= kMaxComponentIncidentIdBytes &&
              occurred_at_ns >= 0 && diagnostic.code == "controller_launch_exited",
          "component_lifecycle_incident_invalid", "incident diagnostic identity is invalid");
  Require(ComponentIncidentDiagnosticIsValid(diagnostic) &&
              (!snapshot || ComponentIncidentDiagnosticSnapshotShapeIsValid(*snapshot)),
          "component_lifecycle_incident_invalid", "incident diagnostic is outside its bound");
  std::lock_guard lock(mutex_);
  RequireOpen();
  Transaction transaction(database_);
  Statement row(database_,
                "SELECT controllable_component_id,containment_state FROM cclc_component_incidents "
                "WHERE target_kind='component' AND incident_id=? AND acknowledged=0");
  row.Bind(1, incident_id);
  if (!row.Step()) return false;
  // Enrichment cannot consume room for a containment receipt not recorded yet.
  if (row.Integer(1) == static_cast<std::int64_t>(ComponentIncidentContainmentStateDTO::kPending))
    return false;
  const auto original = ReadOpenComponentIncidentLocked(ControllableComponentId(row.Text(0)));
  if (!original || original->diagnostic_snapshot || (original->diagnostic && !snapshot))
    return false;
  auto bounded_error = original->diagnostic.value_or(diagnostic);
  const auto* settings = snapshot ? snapshot->find_object_member("settings") : nullptr;
  const auto* configuration =
      settings ? settings->find_object_member("retained_configuration_json") : nullptr;
  if (configuration && configuration->is_string()) {
    Require(bounded_error.details.size() < kMaxLifecycleErrorDetails ||
                bounded_error.details.contains("retained_configuration_sha256"),
            "component_lifecycle_incident_invalid",
            "settings hash exceeds diagnostic detail capacity");
    const auto hash = content_identity::sha256_digest(configuration->as_string());
    const auto existing = bounded_error.details.find("retained_configuration_sha256");
    Require(existing == bounded_error.details.end() || existing->second == hash,
            "component_lifecycle_incident_invalid",
            "capture conflicts with original settings hash");
    bounded_error.details.emplace("retained_configuration_sha256", hash);
  }
  auto bounded_snapshot = snapshot ? *snapshot : content_identity::CanonicalValue(nullptr);
  const auto replace_member = [](const content_identity::CanonicalValue& object,
                                 const std::string_view key,
                                 content_identity::CanonicalValue value) {
    auto fields = object.as_object();
    for (auto& [name, current] : fields) {
      if (name != key) continue;
      current = std::move(value);
      return content_identity::CanonicalValue(std::move(fields));
    }
    fields.emplace_back(std::string(key), std::move(value));
    return content_identity::CanonicalValue(std::move(fields));
  };
  // Reserve actual canonical growth allowed by the existing bounded cause
  // history and public Recover fields before storing immutable evidence.
  // Controls are valid NFC text and encode six bytes, so this uses the codec
  // rather than assuming raw UTF8 bytes equal JSON bytes.
  const std::vector<std::string> maximal_codes(
      kMaxComponentIncidentVendorCodes, std::string(kMaxComponentIncidentVendorCodeBytes, '\x01'));
  auto longest_cause = original->incident.cause_class;
  for (const auto& policy : DefaultLifecycleIncidentRecoveryPolicy())
    if (to_string(policy.cause_class).size() > to_string(longest_cause).size())
      longest_cause = policy.cause_class;
  auto future_primary = original->record_json;
  for (std::size_t change = 0; change < kMaxComponentIncidentTimelineEntries; ++change)
    future_primary = ComponentIncidentRecordWithChange(
        future_primary, std::numeric_limits<std::int64_t>::max(), longest_cause, maximal_codes);
  auto recovered = original->incident;
  recovered.acknowledged = true;
  recovered.acknowledged_by = std::string(kMaxLifecycleRequestLabelBytes, '\x01');
  recovered.acknowledgement_request_id = RequestId(std::string(kMaxLifecycleIdentityBytes, '\x01'));
  recovered.acknowledged_at_ns = std::numeric_limits<std::int64_t>::max();
  future_primary = ComponentIncidentAcknowledgedPayload(future_primary, recovered);
  const auto future_growth = future_primary.size() > original->record_json.size()
                                 ? future_primary.size() - original->record_json.size()
                                 : 0U;
  Require(future_growth < kMaxComponentIncidentRecordBytes, "component_lifecycle_incident_invalid",
          "mandatory incident history exceeds its bound");
  const auto enrichment_budget = kMaxComponentIncidentRecordBytes - future_growth;
  std::string updated;
  std::string event;
  const auto materialize = [&] {
    // Candidate encoding stays local: optional settings may exceed the total
    // budget at ingress. Only the final strictly validated value is persisted.
    using content_identity::CanonicalValue;
    CanonicalValue::Object details;
    for (const auto& [key, value] : bounded_error.details)
      details.emplace_back(key, CanonicalValue(value));
    const CanonicalValue envelope(CanonicalValue::Object{
        {"error",
         CanonicalValue(CanonicalValue::Object{{"code", CanonicalValue(bounded_error.code)},
                                               {"message", CanonicalValue(bounded_error.message)},
                                               {"details", CanonicalValue(std::move(details))}})},
        {"snapshot", bounded_snapshot}});
    const auto attach = [&](const std::string& primary) {
      return content_identity::canonical_json(
          replace_member(content_identity::parse_canonical_json(primary), "diagnostic", envelope));
    };
    updated = attach(original->record_json);
    event = attach(ComponentIncidentRecordWithChange(original->record_json, occurred_at_ns,
                                                     original->incident.cause_class,
                                                     original->incident.vendor_codes));
  };
  const auto oversized = [&] {
    return updated.size() > enrichment_budget || event.size() > kMaxComponentIncidentRecordBytes;
  };
  materialize();
  // DX246: preserve the entire already-bounded tail. Optional settings text
  // is omitted first, explicitly named through the existing ErrorDTO detail.
  std::string omitted;
  while (oversized()) {
    if (bounded_snapshot.is_null()) break;
    auto settings = *bounded_snapshot.find_object_member("settings");
    std::string largest;
    std::size_t largest_bytes{};
    for (const auto& [key, value] : settings.as_object()) {
      if (key == "argv" || !value.is_string()) continue;
      const auto bytes = content_identity::canonical_json(value).size();
      if (bytes > largest_bytes) {
        largest = key;
        largest_bytes = bytes;
      }
    }
    if (largest.empty()) break;
    if (!omitted.empty()) omitted += ',';
    omitted += largest;
    if (bounded_error.details.contains("snapshot_omitted_fields") ||
        bounded_error.details.size() < 8U) {
      bounded_error.details["snapshot_omitted_fields"] = omitted;
    } else {
      Require(false, "component_lifecycle_incident_invalid",
              "omission metadata exceeds original diagnostic detail capacity");
    }
    settings = replace_member(settings, largest, content_identity::CanonicalValue(nullptr));
    bounded_snapshot = replace_member(bounded_snapshot, "settings", std::move(settings));
    materialize();
  }
  if (original->diagnostic) {
    const auto original_omission = original->diagnostic->details.find("snapshot_omitted_fields");
    Require(original_omission == original->diagnostic->details.end() ||
                original_omission->second == omitted,
            "component_lifecycle_incident_invalid",
            "capture conflicts with original omission facts");
  }
  Require(ComponentIncidentDiagnosticIsValid(bounded_error) &&
              (bounded_snapshot.is_null() ||
               ComponentIncidentDiagnosticSnapshotIsValid(bounded_snapshot)) &&
              !oversized(),
          "component_lifecycle_incident_invalid",
          "mandatory incident facts exceed the diagnostic payload bound");
  Statement write(database_,
                  "UPDATE cclc_component_incidents SET record_json=? "
                  "WHERE target_kind='component' AND incident_id=? AND acknowledged=0");
  write.Bind(1, updated);
  write.Bind(2, incident_id);
  Require(!write.Step(), "component_lifecycle_journal_corrupt", "diagnostic write returned a row");
  if (sqlite3_changes(database_) != 1) return false;
  AppendComponentIncidentEventLocked(kComponentIncidentEventKind, incident_id, occurred_at_ns,
                                     event);
  transaction.Commit();
  return true;
}

std::vector<ComponentIncidentEventDTO> SqliteCCLCJournal::Impl::LoadComponentIncidentEvents(
    const std::uint64_t after_sequence, const std::size_t limit) {
  Require(FitsSqliteInteger(after_sequence) && limit > 0U &&
              limit <= kMaxComponentIncidentEventPage,
          "component_lifecycle_incident_invalid", "incident event page is invalid");
  std::lock_guard lock(mutex_);
  RequireOpen();
  Statement rows(database_,
                 "SELECT event_sequence,event_kind,incident_id,occurred_at_ns,payload_json "
                 "FROM cclc_component_incident_events WHERE event_sequence > ? "
                 "ORDER BY event_sequence LIMIT ?");
  rows.Bind(1, static_cast<std::int64_t>(after_sequence));
  rows.Bind(2, static_cast<std::int64_t>(limit));
  std::vector<ComponentIncidentEventDTO> result;
  while (rows.Step()) {
    Require(rows.Text(4).size() <= kMaxComponentIncidentRecordBytes,
            "component_lifecycle_journal_corrupt", "incident event exceeds its bound");
    result.push_back({Positive(rows.Integer(0), "incident event sequence"), rows.Text(1),
                      rows.Text(2), rows.Integer(3), rows.Text(4)});
  }
  return result;
}

void SqliteCCLCJournal::Impl::CompactTerminalResults(std::size_t prospective_terminal_count,
                                                     std::uint64_t prospective_terminal_bytes) {
  Require(prospective_terminal_count <= 1U, "component_lifecycle_journal_corrupt",
          "terminal CCLC compaction prospect is invalid");
  while (true) {
    Statement retained(database_, "SELECT count(*),coalesce(sum(retained_bytes),0) FROM (" +
                                      std::string(kSettledResults) + ")");
    Require(retained.Step(), "component_lifecycle_journal_corrupt",
            "terminal CCLC result accounting is missing");
    const auto terminal_count = Nonnegative(retained.Integer(0), "terminal CCLC result count");
    const auto terminal_bytes = Nonnegative(retained.Integer(1), "terminal CCLC result bytes");
    Require(!retained.Step(), "component_lifecycle_journal_corrupt",
            "terminal CCLC result accounting is duplicated");
    const bool count_fits =
        terminal_count <= exact_result_history_capacity_ -
                              std::min(exact_result_history_capacity_, prospective_terminal_count);
    const bool bytes_fit =
        prospective_terminal_bytes <= terminal_result_bytes_capacity_ &&
        terminal_bytes <= terminal_result_bytes_capacity_ - prospective_terminal_bytes;
    if (count_fits && bytes_fit) {
      RetireQuiescentRequestNamespaces();
      return;
    }

    if (terminal_count == 0U) {
      return;
    }

    Statement oldest(database_,
                     "SELECT request_kind,transaction_sequence,membership_sequence,"
                     "terminal_sequence FROM (" +
                         std::string(kSettledResults) + ") ORDER BY terminal_sequence LIMIT 1");
    Require(oldest.Step(), "component_lifecycle_journal_corrupt",
            "oldest terminal CCLC result is missing");
    const auto request_kind = oldest.Text(0);
    const auto sequence = Positive(oldest.Integer(1), "transaction sequence");
    const auto membership_sequence = Positive(oldest.Integer(2), "request membership sequence");
    (void)Positive(oldest.Integer(3), "terminal sequence");
    Require(!oldest.Step(), "component_lifecycle_journal_corrupt",
            "oldest terminal CCLC result query returned multiple rows");

    Require(request_kind == "component_lifecycle" || request_kind == "route_selection" ||
                request_kind == "rejection" || request_kind == "lifecycle_target_retirement",
            "component_lifecycle_journal_corrupt",
            "oldest terminal CCLC result has an invalid request kind");
    if (request_kind == "component_lifecycle") {
      Statement erase_claim(
          database_,
          "DELETE FROM cclc_lifecycle_cell_claims WHERE "
          "lifecycle_transaction_id=(SELECT lifecycle_transaction_id FROM "
          "lifecycle_transactions WHERE transaction_sequence=?) "
          "AND release_receipt_recorded=1");
      erase_claim.Bind(1, static_cast<std::int64_t>(sequence));
      Require(!erase_claim.Step(),
              "component_lifecycle_journal_corrupt",
              "terminal lifecycle claim retirement returned a row");
    }
    if (request_kind == "component_lifecycle" || request_kind == "route_selection") {
      Statement erase(
          database_, request_kind == "component_lifecycle"
                         ? "DELETE FROM lifecycle_transactions WHERE transaction_sequence=?"
                         : "DELETE FROM route_selection_transactions WHERE transaction_sequence=?");
      erase.Bind(1, static_cast<std::int64_t>(sequence));
      Require(!erase.Step() && sqlite3_changes(database_) == 1,
              "component_lifecycle_journal_corrupt",
              "terminal CCLC result compaction did not delete one row");
    }
    Statement tombstone(database_,
                        "UPDATE cclc_request_memberships SET "
                        "retained_result=0,rejection_blob=NULL,rejection_sequence=NULL, "
                        "target_retirement_blob=NULL,target_retirement_sequence=NULL "
                        "WHERE membership_sequence=?");
    tombstone.Bind(1, static_cast<std::int64_t>(membership_sequence));
    Require(!tombstone.Step() && sqlite3_changes(database_) == 1,
            "component_lifecycle_journal_corrupt",
            "CCLC request membership compaction did not update one row");
  }
}

SqliteCCLCJournal::SqliteCCLCJournal(
    std::filesystem::path database_path, CellId cell_id, std::size_t exact_result_history_capacity,
    std::size_t terminal_result_bytes_capacity,
    std::function<void(const ComponentIncidentPruneNotice&)> on_incidents_pruned,
    NativeApplyParentPreparationFailureSource preparation_failure_source)
    : impl_(std::make_unique<Impl>(std::move(database_path), std::move(cell_id),
                                   exact_result_history_capacity, terminal_result_bytes_capacity,
                                   std::move(on_incidents_pruned),
                                   std::move(preparation_failure_source))) {}

SqliteCCLCJournal::~SqliteCCLCJournal() = default;

Result<CCLCJournalRecoveryDTO> SqliteCCLCJournal::BeginOwnerEpoch(
    const RequestOwnerEpoch& owner_epoch, std::int64_t started_at_ns,
    std::size_t request_membership_capacity,
    const std::optional<RequestOwnerEpoch>& quiescent_predecessor) {
  return BeginOwnerEpoch(owner_epoch, started_at_ns, request_membership_capacity,
                         quiescent_predecessor, CCLCOwnerEpochTransitionPurpose::kRestart);
}

Result<CCLCJournalRecoveryDTO> SqliteCCLCJournal::BeginOwnerEpoch(
    const RequestOwnerEpoch& owner_epoch, std::int64_t started_at_ns,
    std::size_t request_membership_capacity,
    const std::optional<RequestOwnerEpoch>& quiescent_predecessor,
    CCLCOwnerEpochTransitionPurpose purpose) {
  return AsResult<CCLCJournalRecoveryDTO>([&]() {
    return impl_->BeginOwnerEpoch(owner_epoch, started_at_ns, request_membership_capacity,
                                  quiescent_predecessor, purpose);
  });
}

Result<void> SqliteCCLCJournal::ReadQuiescentOwner(const RequestOwnerEpoch& expected) {
  return AsResult<void>([&] { impl_->ReadQuiescentOwner(expected); });
}

Result<std::optional<CCLCLifecycleRequestResultDTO>> SqliteCCLCJournal::InspectLifecycleRequest(
    const RequestOwnerEpoch& active_owner_epoch, const ComponentLifecycleIntentDTO& intent) {
  return AsResult<std::optional<CCLCLifecycleRequestResultDTO>>(
      [&]() { return impl_->InspectLifecycleRequest(active_owner_epoch, intent); });
}

Result<CCLCLifecycleRequestResultDTO> SqliteCCLCJournal::AcceptOrReplayLifecycle(
    const RequestOwnerEpoch& active_owner_epoch, const ComponentLifecycleIntentDTO& intent,
    const ComponentLifecycleAcceptedDTO& candidate_acceptance,
    const ComponentLifecycleStatusDTO& candidate_initial_status) {
  return AsResult<CCLCLifecycleRequestResultDTO>([&]() {
    return impl_->AcceptOrReplayLifecycle(active_owner_epoch, intent, candidate_acceptance,
                                          candidate_initial_status);
  });
}

Result<CCLCLifecycleRequestResultDTO> SqliteCCLCJournal::AcceptOrReplayTargetLifecycle(
    const RequestOwnerEpoch& owner, const ComponentLifecycleIntentDTO& intent,
    const ComponentLifecycleAcceptedDTO& acceptance, const ComponentLifecycleStatusDTO& status,
    const ComponentLifecycleTargetExpectationDTO& target, const CellAdmissionClaimDTO& claim) {
  return AsResult<CCLCLifecycleRequestResultDTO>([&] {
    return impl_->AcceptOrReplayLifecycle(owner, intent, acceptance, status, nullptr, false,
                                         &target, &claim);
  });
}

Result<std::optional<CCLCLifecycleRequestResultDTO>>
SqliteCCLCJournal::InspectSelectedComponentBringUpRequest(const RequestOwnerEpoch& owner,
                                                          const ComponentLifecycleIntentDTO& intent,
                                                          const ContentDigest& digest) {
  return AsResult<std::optional<CCLCLifecycleRequestResultDTO>>(
      [&] { return impl_->InspectLifecycleRequest(owner, intent, &digest); });
}

Result<std::optional<CCLCLifecycleRequestResultDTO>>
SqliteCCLCJournal::ReadSelectedComponentBringUpRequestResult(
    const ComponentLifecycleIntentDTO& intent, const ContentDigest& digest) {
  return AsResult<std::optional<CCLCLifecycleRequestResultDTO>>(
      [&] { return impl_->ReadLifecycleRequestResult(intent, &digest); });
}

Result<CCLCLifecycleRequestResultDTO> SqliteCCLCJournal::AcceptOrReplaySelectedComponentBringUp(
    const RequestOwnerEpoch& owner, const ComponentLifecycleIntentDTO& intent,
    const ComponentLifecycleAcceptedDTO& acceptance, const ComponentLifecycleStatusDTO& status,
    const ContentDigest& original_request_digest) {
  return AsResult<CCLCLifecycleRequestResultDTO>([&] {
    return impl_->AcceptOrReplayLifecycle(owner, intent, acceptance, status,
                                          &original_request_digest);
  });
}

Result<std::optional<CCLCLifecycleRequestResultDTO>>
SqliteCCLCJournal::InspectApplyComponentBringUpRequest(const RequestOwnerEpoch& owner,
    const ComponentLifecycleIntentDTO& intent, const ContentDigest& digest) {
  return AsResult<std::optional<CCLCLifecycleRequestResultDTO>>(
      [&] { return impl_->InspectLifecycleRequest(owner, intent, &digest, true); });
}

Result<std::optional<CCLCLifecycleRequestResultDTO>>
SqliteCCLCJournal::ReadApplyComponentBringUpRequestResult(
    const ComponentLifecycleIntentDTO& intent, const ContentDigest& digest) {
  return AsResult<std::optional<CCLCLifecycleRequestResultDTO>>(
      [&] { return impl_->ReadLifecycleRequestResult(intent, &digest, true); });
}

Result<ApplyComponentGenerationSelectionDTO> SqliteCCLCJournal::ReadApplyComponentGenerationSelection(
    const RequestOwnerEpoch& owner, const ControllableComponentId& component,
    const std::optional<ApplyComponentQualifiedPredecessorDTO>& qualified) {
  return AsResult<ApplyComponentGenerationSelectionDTO>(
      [&] { return impl_->ReadApplyComponentGenerationSelection(owner, component, qualified); });
}

Result<CCLCLifecycleRequestResultDTO> SqliteCCLCJournal::AcceptOrReplayApplyComponentBringUp(
    const RequestOwnerEpoch& owner, const ComponentLifecycleIntentDTO& intent,
    const ComponentLifecycleAcceptedDTO& acceptance, const ComponentLifecycleStatusDTO& status,
    const ContentDigest& digest) {
  return AsResult<CCLCLifecycleRequestResultDTO>([&] {
    return impl_->AcceptOrReplayLifecycle(owner, intent, acceptance, status, &digest, true);
  });
}

Result<void> SqliteCCLCJournal::RecordSelectedCellClaimAttempt(
    const RequestOwnerEpoch& owner, const CellAdmissionClaimIntentDTO& intent,
    const ContentDigest& original_request_digest) {
  return AsResult<void>([&] {
    Require(intent.owner_transaction_id.starts_with("selected-lifecycle."),
            "component_lifecycle_claim_invalid", "selected attempt requires selected claim intent");
    impl_->RecordCellClaimAttempt(owner, intent, original_request_digest);
  });
}

Result<CCLCLifecycleRequestResultDTO> SqliteCCLCJournal::RecordSelectedComponentBringUpRejection(
    const RequestOwnerEpoch& owner, const ComponentLifecycleIntentDTO& intent,
    const ContentDigest& original_request_digest, const ErrorDTO& error,
    const std::optional<CellAdmissionClaimIntentDTO>& unacquired) {
  return AsResult<CCLCLifecycleRequestResultDTO>([&] {
    return impl_->RecordLifecycleRejection(owner, intent, error, unacquired,
                                           &original_request_digest);
  });
}

Result<void> SqliteCCLCJournal::RecordCellClaimAttempt(const RequestOwnerEpoch& owner,
                                                       const CellAdmissionClaimIntentDTO& intent) {
  return AsResult<void>([&]() { impl_->RecordCellClaimAttempt(owner, intent); });
}

Result<std::optional<CCLCLifecycleRequestResultDTO>> SqliteCCLCJournal::ReadLifecycleRequestResult(
    const ComponentLifecycleIntentDTO& intent) {
  return AsResult<std::optional<CCLCLifecycleRequestResultDTO>>(
      [&]() { return impl_->ReadLifecycleRequestResult(intent); });
}

Result<CCLCLifecycleRequestResultDTO> SqliteCCLCJournal::RecordLifecycleRejection(
    const RequestOwnerEpoch& owner, const ComponentLifecycleIntentDTO& intent,
    const ErrorDTO& error, const std::optional<CellAdmissionClaimIntentDTO>& unacquired) {
  return AsResult<CCLCLifecycleRequestResultDTO>(
      [&]() { return impl_->RecordLifecycleRejection(owner, intent, error, unacquired); });
}

Result<void> SqliteCCLCJournal::RecordCellClaimAcquired(const RequestOwnerEpoch& owner,
                                                        const CellAdmissionClaimDTO& claim) {
  return AsResult<void>([&]() { impl_->RecordCellClaimAcquired(owner, claim); });
}

Result<std::uint64_t>
SqliteCCLCJournal::RecordLifecycleTerminalAndClaimRelease(
    const RequestOwnerEpoch& owner, const ComponentLifecycleStatusDTO& status,
    const CellAdmissionClaimReleaseDTO& release) {
  return AsResult<std::uint64_t>(
      [&]() { return impl_->RecordLifecycleStatus(owner, status, &release); });
}

Result<void> SqliteCCLCJournal::RecordCellClaimReleaseReceipt(
    const RequestOwnerEpoch& owner, const CellAdmissionClaimReleaseDTO& release,
    const CellAdmissionRecoveryReceiptDTO& receipt) {
  return AsResult<void>([&]() { impl_->RecordCellClaimReleaseReceipt(owner, release, receipt); });
}

Result<void> SqliteCCLCJournal::RecordUnacceptedCellClaimRelease(
    const RequestOwnerEpoch& owner, const CellAdmissionClaimReleaseDTO& release) {
  return AsResult<void>([&]() { impl_->RecordUnacceptedCellClaimRelease(owner, release); });
}

Result<void> SqliteCCLCJournal::RecordCellClaimNotAcquired(
    const RequestOwnerEpoch& owner, const CellAdmissionClaimIntentDTO& intent) {
  return AsResult<void>([&]() { impl_->RecordCellClaimNotAcquired(owner, intent); });
}

Result<std::uint64_t> SqliteCCLCJournal::RecordEffectPrepared(
    const RequestOwnerEpoch& active_owner_epoch, const CCLCEffectAttemptDTO& prepared_attempt,
    const ComponentLifecycleStatusDTO& successor_status) {
  return AsResult<std::uint64_t>([&]() {
    return impl_->RecordEffectPrepared(active_owner_epoch, prepared_attempt, successor_status);
  });
}

Result<std::uint64_t> SqliteCCLCJournal::RecordEffectCompleted(
    const RequestOwnerEpoch& active_owner_epoch, const CCLCEffectAttemptDTO& completed_attempt,
    const ComponentLifecycleStatusDTO& successor_status) {
  return AsResult<std::uint64_t>([&]() {
    return impl_->RecordEffectCompleted(active_owner_epoch, completed_attempt, successor_status);
  });
}

Result<std::uint64_t> SqliteCCLCJournal::RecordLifecycleStatus(
    const RequestOwnerEpoch& active_owner_epoch, const ComponentLifecycleStatusDTO& status) {
  return AsResult<std::uint64_t>(
      [&]() { return impl_->RecordLifecycleStatus(active_owner_epoch, status); });
}

Result<std::optional<ComponentLifecycleStatusDTO>> SqliteCCLCJournal::LoadRetainedLifecycleStatus(
    const LifecycleTransactionId& lifecycle_transaction_id) {
  return AsResult<std::optional<ComponentLifecycleStatusDTO>>(
      [&]() { return impl_->LoadRetainedLifecycleStatus(lifecycle_transaction_id); });
}

Result<ComponentLifecycleCurrentFactReadbackDTO>
SqliteCCLCJournal::LoadCurrentComponentLifecycleFact(
    const ControllableComponentId& controllable_component_id) {
  return AsResult<ComponentLifecycleCurrentFactReadbackDTO>(
      [&]() { return impl_->LoadCurrentComponentLifecycleFact(controllable_component_id); });
}

Result<CCLCCurrentFactsReadbackDTO> SqliteCCLCJournal::LoadCurrentSnapshotFacts(
    const std::vector<ControllableComponentId>& canonical_controllable_component_ids,
    const std::vector<ControlDomainId>& canonical_control_domain_ids) {
  return AsResult<CCLCCurrentFactsReadbackDTO>([&]() {
    return impl_->LoadCurrentSnapshotFacts(canonical_controllable_component_ids,
                                           canonical_control_domain_ids);
  });
}

Result<std::optional<CCLCRouteSelectionRequestResultDTO>>
SqliteCCLCJournal::InspectRouteSelectionRequest(const RequestOwnerEpoch& active_owner_epoch,
                                                const RouteSelectionIntentDTO& intent) {
  return AsResult<std::optional<CCLCRouteSelectionRequestResultDTO>>(
      [&]() { return impl_->InspectRouteSelectionRequest(active_owner_epoch, intent); });
}

Result<std::optional<CCLCRouteSelectionRequestResultDTO>>
SqliteCCLCJournal::ReadRouteSelectionRequestResult(const RouteSelectionIntentDTO& intent) {
  return AsResult<std::optional<CCLCRouteSelectionRequestResultDTO>>(
      [&]() { return impl_->ReadRouteSelectionRequestResult(intent); });
}

Result<CCLCRouteSelectionRequestResultDTO> SqliteCCLCJournal::AcceptOrReplayRouteSelection(
    const RequestOwnerEpoch& active_owner_epoch, const RouteSelectionIntentDTO& intent,
    const RouteSelectionAcceptedDTO& candidate_acceptance,
    const RouteSelectionStatusDTO& candidate_initial_status) {
  return AsResult<CCLCRouteSelectionRequestResultDTO>([&]() {
    return impl_->AcceptOrReplayRouteSelection(active_owner_epoch, intent, candidate_acceptance,
                                               candidate_initial_status);
  });
}

Result<CCLCRouteSelectionRequestResultDTO> SqliteCCLCJournal::RecordRouteSelectionRejection(
    const RequestOwnerEpoch& owner, const RouteSelectionIntentDTO& intent, const ErrorDTO& error,
    const std::optional<CellAdmissionClaimIntentDTO>& unacquired) {
  return AsResult<CCLCRouteSelectionRequestResultDTO>(
      [&] { return impl_->RecordRouteSelectionRejection(owner, intent, error, unacquired); });
}

Result<std::uint64_t> SqliteCCLCJournal::RecordRouteEffectPrepared(
    const RequestOwnerEpoch& active_owner_epoch, const CCLCRouteEffectAttemptDTO& prepared_attempt,
    const RouteSelectionStatusDTO& successor_status) {
  return AsResult<std::uint64_t>([&]() {
    return impl_->RecordRouteEffectPrepared(active_owner_epoch, prepared_attempt, successor_status);
  });
}

Result<std::uint64_t> SqliteCCLCJournal::RecordRouteEffectCompleted(
    const RequestOwnerEpoch& active_owner_epoch, const CCLCRouteEffectAttemptDTO& completed_attempt,
    const RouteSelectionStatusDTO& successor_status) {
  return AsResult<std::uint64_t>([&]() {
    return impl_->RecordRouteEffectCompleted(active_owner_epoch, completed_attempt,
                                             successor_status);
  });
}

Result<std::uint64_t> SqliteCCLCJournal::RecordRouteSelectionStatus(
    const RequestOwnerEpoch& active_owner_epoch, const RouteSelectionStatusDTO& status) {
  return AsResult<std::uint64_t>(
      [&]() { return impl_->RecordRouteSelectionStatus(active_owner_epoch, status); });
}

Result<std::uint64_t> SqliteCCLCJournal::RecordRouteEffectCompletedAndClaimRelease(
    const RequestOwnerEpoch& owner, const CCLCRouteEffectAttemptDTO& completed,
    const RouteSelectionStatusDTO& status, const CellAdmissionClaimReleaseDTO& release) {
  return AsResult<std::uint64_t>(
      [&] { return impl_->RecordRouteEffectCompleted(owner, completed, status, &release); });
}

Result<std::uint64_t> SqliteCCLCJournal::RecordRouteTerminalAndClaimRelease(
    const RequestOwnerEpoch& owner, const RouteSelectionStatusDTO& status,
    const CellAdmissionClaimReleaseDTO& release) {
  return AsResult<std::uint64_t>(
      [&] { return impl_->RecordRouteSelectionStatus(owner, status, &release); });
}

Result<std::optional<RouteSelectionStatusDTO>> SqliteCCLCJournal::LoadRetainedRouteSelectionStatus(
    const RouteSelectionTransactionId& route_selection_transaction_id) {
  return AsResult<std::optional<RouteSelectionStatusDTO>>(
      [&]() { return impl_->LoadRetainedRouteSelectionStatus(route_selection_transaction_id); });
}

Result<std::optional<ActiveControlConfigurationDTO>>
SqliteCCLCJournal::LoadCurrentActiveControlConfiguration(const ControlDomainId& control_domain_id) {
  return AsResult<std::optional<ActiveControlConfigurationDTO>>(
      [&]() { return impl_->LoadCurrentActiveControlConfiguration(control_domain_id); });
}

Result<NativeApplyPlanDTO> SqliteCCLCJournal::PlanNativeApply(
    const NativeApplyPlanRequestDTO& request) {
  return AsResult<NativeApplyPlanDTO>([&]() { return impl_->PlanNativeApply(request); });
}

Result<std::optional<NamedApplyPublicationTransactionDTO>>
SqliteCCLCJournal::ReadUnsettledNativeApplyPlan() {
  return AsResult<std::optional<NamedApplyPublicationTransactionDTO>>(
      [&]() { return impl_->ReadUnsettledNativeApplyPlan(); });
}

Result<std::vector<ControllableComponentId>>
SqliteCCLCJournal::LoadComponentsWithUnsettledLifecycle() {
  return AsResult<std::vector<ControllableComponentId>>(
      [&]() { return impl_->LoadComponentsWithUnsettledLifecycle(); });
}

Result<std::optional<NativeApplyPlanDTO>> SqliteCCLCJournal::LoadNativeApplyPlan(
    const NamedApplyPublicationTransactionDTO& apply) {
  return AsResult<std::optional<NativeApplyPlanDTO>>(
      [&]() { return impl_->LoadNativeApplyPlan(apply); });
}

Result<void> SqliteCCLCJournal::AdoptNativeApplyFacts(
    const NativeApplyFactAdoptionDTO& adoption) {
  return AsResult<void>([&]() { impl_->AdoptNativeApplyFacts(adoption); });
}

Result<bool> SqliteCCLCJournal::HasNativeApplyFacts(
    const ActiveBundlePublicationReceiptDTO& publication) {
  return AsResult<bool>([&]() { return impl_->HasNativeApplyFacts(publication); });
}

Result<ComponentLifecycleTargetDTO> SqliteCCLCJournal::SetLifecycleTarget(
    const ControllableComponentId& component, const LifecycleTargetStateDTO requested_state,
    const LifecycleTargetOriginDTO origin) {
  return AsResult<ComponentLifecycleTargetDTO>(
      [&]() { return impl_->SetLifecycleTarget(component, requested_state, origin); });
}

Result<std::vector<ComponentLifecycleTargetDTO>> SqliteCCLCJournal::LoadLifecycleTargets() {
  return AsResult<std::vector<ComponentLifecycleTargetDTO>>(
      [&]() { return impl_->LoadLifecycleTargets(); });
}

Result<std::optional<ComponentLifecycleTargetDTO>> SqliteCCLCJournal::LoadLifecycleTarget(
    const ControllableComponentId& component) {
  return AsResult<std::optional<ComponentLifecycleTargetDTO>>(
      [&]() { return impl_->LoadLifecycleTarget(component); });
}

Result<std::optional<ComponentLifecycleTargetRetentionDTO>>
SqliteCCLCJournal::ReadLifecycleTargetRetention(const ControllableComponentId& component) {
  return AsResult<std::optional<ComponentLifecycleTargetRetentionDTO>>(
      [&] { return impl_->ReadLifecycleTargetRetention(component); });
}

Result<ComponentLifecycleTargetFactsDTO> SqliteCCLCJournal::LoadLifecycleTargetFacts(
    const ControllableComponentId& component) {
  return AsResult<ComponentLifecycleTargetFactsDTO>(
      [&]() { return impl_->LoadLifecycleTargetFacts(component); });
}

Result<bool> SqliteCCLCJournal::RecordInterventionCause(
    const ControllableComponentId& component, const ControlDomainId& domain,
    const std::optional<LifecycleBlockingCauseClassDTO> cause_class,
    const std::int64_t observed_at_ns, const std::uint64_t recovery_generation) {
  return AsResult<bool>([&]() {
    return impl_->RecordInterventionCause(component, domain, cause_class, observed_at_ns,
                                          recovery_generation);
  });
}

Result<std::vector<ComponentLifecycleInterventionCauseDTO>>
SqliteCCLCJournal::LoadInterventionCauses(const ControllableComponentId& component) {
  return AsResult<std::vector<ComponentLifecycleInterventionCauseDTO>>(
      [&]() { return impl_->LoadInterventionCauses(component); });
}

Result<bool> SqliteCCLCJournal::RecordLifecycleTargetReached(
    const ControllableComponentId& component, const bool reached,
    const std::uint64_t recovery_generation) {
  return AsResult<bool>([&]() {
    return impl_->RecordLifecycleTargetReached(component, reached, recovery_generation);
  });
}

Result<bool> SqliteCCLCJournal::RecordLifecycleTargetReached(
    const ComponentLifecycleTargetExpectationDTO& target, bool reached, std::uint64_t recovery) {
  return AsResult<bool>([&] {
    return impl_->RecordLifecycleTargetReached(target.controllable_component_id, reached, recovery, &target);
  });
}

Result<std::vector<ComponentLifecycleTargetDTO>> SqliteCCLCJournal::RetireLifecycleTargetsForClose(
    const RequestOwnerEpoch& owner) {
  return AsResult<std::vector<ComponentLifecycleTargetDTO>>([&] {
    return impl_->RetireLifecycleTargetsForClose(owner);
  });
}

Result<bool> SqliteCCLCJournal::ComponentInUnadoptedApplyPlan(
    const ControllableComponentId& component) {
  return AsResult<bool>([&]() { return impl_->ComponentInUnadoptedApplyPlan(component); });
}

Result<CellObserverIncidentRecordDTO> SqliteCCLCJournal::RecordCellObserverIncident(
    const CellObserverIncidentRecordDTO& record) {
  return AsResult<CellObserverIncidentRecordDTO>(
      [&] { return impl_->RecordCellObserverIncident(record); });
}

Result<std::optional<CellObserverIncidentRecordDTO>>
SqliteCCLCJournal::LoadOpenCellObserverIncident() {
  return AsResult<std::optional<CellObserverIncidentRecordDTO>>(
      [&] { return impl_->LoadOpenCellObserverIncident(); });
}

Result<std::optional<CellObserverLossAssociationDTO>>
SqliteCCLCJournal::LoadOpenCellObserverLossAssociation() {
  return AsResult<std::optional<CellObserverLossAssociationDTO>>(
      [&] { return impl_->LoadOpenCellObserverLossAssociation(); });
}

Result<std::vector<ComponentLifecycleTargetDTO>> SqliteCCLCJournal::LoadLifecycleTargetsForClose(
    const RequestOwnerEpoch& owner) {
  return AsResult<std::vector<ComponentLifecycleTargetDTO>>([&] { return impl_->LoadLifecycleTargetsForClose(owner); });
}

Result<std::optional<CCLCLifecycleTargetRetirementResultDTO>>
SqliteCCLCJournal::ReadLifecycleTargetRetirementResult(const CCLCLifecycleTargetRetirementIntentDTO& intent) {
  return AsResult<std::optional<CCLCLifecycleTargetRetirementResultDTO>>(
      [&] { return impl_->ReadLifecycleTargetRetirementResult(intent); });
}

Result<CCLCLifecycleTargetRetirementResultDTO> SqliteCCLCJournal::RetireLifecycleTargetsForClose(
    const RequestOwnerEpoch& owner, const CCLCLifecycleTargetRetirementIntentDTO& intent,
    const ICellCloseAdmissionLease& lease) {
  return AsResult<CCLCLifecycleTargetRetirementResultDTO>(
      [&] { return impl_->RetireLifecycleTargetsForClose(owner, intent, lease); });
}

Result<std::optional<CellObserverBringUpRecordDTO>>
SqliteCCLCJournal::LoadLatestCellObserverBringUp() {
  return AsResult<std::optional<CellObserverBringUpRecordDTO>>(
      [&] { return impl_->LoadLatestCellObserverBringUp(); });
}
Result<CellObserverBringUpAdmissionDTO> SqliteCCLCJournal::BeginLatestCellObserverBringUp(
    const CellObserverBringUpRecordDTO& record,
    const std::optional<CellObserverBringUpRecordDTO>& expected) {
  return AsResult<CellObserverBringUpAdmissionDTO>(
      [&] { return impl_->BeginLatestCellObserverBringUp(record, expected); });
}
Result<CellObserverBringUpRecordDTO> SqliteCCLCJournal::UpdateLatestCellObserverBringUp(
    const CellObserverBringUpRecordDTO& expected, const CellObserverBringUpRecordDTO& updated) {
  return AsResult<CellObserverBringUpRecordDTO>(
      [&] { return impl_->UpdateLatestCellObserverBringUp(expected, updated); });
}
Result<CellObserverBringUpRecordDTO> SqliteCCLCJournal::CloseCellObserverIncidentByOperatorBringUp(
    const CellObserverBringUpRecordDTO& expected, const CellObserverBringUpOpenDTO& open) {
  return AsResult<CellObserverBringUpRecordDTO>(
      [&] { return impl_->CloseCellObserverIncidentByOperatorBringUp(expected, open); });
}

Result<ComponentIncidentRecordDTO> SqliteCCLCJournal::OpenComponentIncident(
    const ComponentIncidentRecordDTO& record, const std::uint64_t recovery_generation) {
  return AsResult<ComponentIncidentRecordDTO>(
      [&]() { return impl_->OpenComponentIncident(record, recovery_generation); });
}

Result<std::optional<ComponentIncidentRecordDTO>> SqliteCCLCJournal::LoadOpenComponentIncident(
    const ControllableComponentId& component) {
  return AsResult<std::optional<ComponentIncidentRecordDTO>>(
      [&]() { return impl_->LoadOpenComponentIncident(component); });
}

Result<bool> SqliteCCLCJournal::RecordComponentIncidentContainment(
    const std::string& incident_id, const ComponentIncidentContainmentStateDTO state,
    const std::string& record_json, const std::int64_t occurred_at_ns) {
  return AsResult<bool>([&]() {
    return impl_->RecordComponentIncidentContainment(incident_id, state, record_json,
                                                     occurred_at_ns);
  });
}

Result<std::optional<CCLCLifecycleRequestResultDTO>> SqliteCCLCJournal::RecoverComponentIncident(
    const RequestOwnerEpoch& active_owner_epoch, const ComponentLifecycleIntentDTO& intent,
    const std::string& acknowledged_by, const std::int64_t acknowledged_at_ns) {
  return AsResult<std::optional<CCLCLifecycleRequestResultDTO>>([&]() {
    return impl_->RecoverComponentIncident(active_owner_epoch, intent, acknowledged_by,
                                           acknowledged_at_ns);
  });
}

Result<bool> SqliteCCLCJournal::UpdateOpenComponentIncident(
    const std::string& incident_id, const LifecycleBlockingCauseClassDTO cause_class,
    const std::vector<std::string>& vendor_codes, const std::string& record_json) {
  return AsResult<bool>([&]() {
    return impl_->UpdateOpenComponentIncident(incident_id, cause_class, vendor_codes, record_json);
  });
}

Result<bool> SqliteCCLCJournal::AppendComponentIncidentDiagnostic(
    const std::string& incident_id, const std::int64_t occurred_at_ns, const ErrorDTO& diagnostic,
    std::shared_ptr<const content_identity::CanonicalValue> snapshot) {
  return AsResult<bool>([&]() {
    return impl_->AppendComponentIncidentDiagnostic(incident_id, occurred_at_ns, diagnostic,
                                                    std::move(snapshot));
  });
}

Result<std::vector<ComponentIncidentEventDTO>> SqliteCCLCJournal::LoadComponentIncidentEvents(
    const std::uint64_t after_sequence, const std::size_t limit) {
  return AsResult<std::vector<ComponentIncidentEventDTO>>(
      [&]() { return impl_->LoadComponentIncidentEvents(after_sequence, limit); });
}

void SqliteCCLCJournal::Close() {
  impl_->Close();
}

namespace {

// One exact read covers either CCLC family and the accepted/never-accepted
// boundary. No claim is manufactured for legacy route records.
CellAdmissionClaimReleaseDTO ReadSettledCCLCClaim(sqlite3* database,
                                                  const CellAdmissionClaimDTO& claim) {
  Statement row(
      database,
      "SELECT c.owner_epoch,c.request_id,c.request_payload_digest,c.expected_admission_generation,"
      "c.claim_generation,c.acquired_admission_generation,c.accepted_at_ns,c.lifecycle_accepted,"
      "c.release_kind,c.release_evidence_digest,c.release_receipt_recorded,"
      "t.terminal,t.status_blob,r.terminal,r.status_blob,"
      "(SELECT count(*) FROM cclc_effect_attempts e WHERE "
      "e.lifecycle_transaction_id=c.lifecycle_transaction_id"
      " AND e.completed_attempt_blob IS NULL)+(SELECT count(*) FROM cclc_route_effect_attempts e"
      " WHERE e.route_selection_transaction_id=c.lifecycle_transaction_id AND "
      "e.completed_attempt_blob IS NULL),"
      "c.program_node_evidence,m.request_kind,m.semantic_request_digest,"
      "(SELECT count(*) FROM cclc_effect_attempts e WHERE "
      "e.lifecycle_transaction_id=c.lifecycle_transaction_id)+"
      "(SELECT count(*) FROM cclc_route_effect_attempts e WHERE "
      "e.route_selection_transaction_id=c.lifecycle_transaction_id),c.recovery_configuration "
      "FROM cclc_lifecycle_cell_claims c LEFT JOIN lifecycle_transactions t ON "
      "t.lifecycle_transaction_id=c.lifecycle_transaction_id "
      "LEFT JOIN route_selection_transactions r ON "
      "r.route_selection_transaction_id=c.lifecycle_transaction_id "
      "LEFT JOIN cclc_request_memberships m ON m.owner_epoch=c.owner_epoch AND "
      "m.request_id=c.request_id "
      "WHERE c.lifecycle_transaction_id=?");
  row.Bind(1, claim.intent.owner_transaction_id);
  Require(row.Step() && !row.IsNull(8) && !row.IsNull(9),
          "component_lifecycle_claim_settlement_missing", "CCLC claim has no durable settlement");
  const auto node = row.IsNull(16) ? std::optional<ProgramNodeAdmissionEvidenceDTO>{}
                                   : detail::DecodeProgramNodeAdmissionEvidence(row.Blob(16));
  const auto recovery = row.IsNull(20) ? std::optional<std::string>{} : row.Blob(20);
  std::optional<ContentDigest> original_digest;
  if (claim.intent.owner_transaction_id.starts_with("selected-lifecycle.")) {
    original_digest = OriginalRequestDigestFromRecovery(claim.intent, recovery);
  } else {
    Require(recovery == RecoveryConfigurationBytes(claim.intent),
            "component_lifecycle_claim_settlement_invalid", "CCLC recovery scope differs");
  }
  const auto kind = static_cast<CellAdmissionSettlementKindDTO>(row.Integer(8));
  const std::string expected_family =
      claim.intent.owner_transaction_id.starts_with("route-selection.") ? "route_selection"
                                                                        : "component_lifecycle";
  Require(row.Text(0) == claim.intent.owner_epoch.value() &&
              row.Text(1) == claim.intent.request_id.value() &&
              row.Text(2) == claim.intent.request_payload_digest.value() &&
              row.Integer(3) ==
                  static_cast<std::int64_t>(claim.intent.expected_admission_generation.value()) &&
              row.Integer(4) == static_cast<std::int64_t>(claim.claim_generation.value()) &&
              row.Integer(5) ==
                  static_cast<std::int64_t>(claim.acquired_admission_generation.value()) &&
              row.Integer(6) == claim.accepted_at_ns && row.Integer(10) == 0 &&
              row.Integer(15) == 0 && node == claim.intent.program_node_evidence &&
              row.Text(17) == expected_family &&
              row.Text(18) == original_digest.value_or(claim.intent.request_payload_digest).value(),
          "component_lifecycle_claim_settlement_invalid",
          "CCLC settlement does not match the held claim");
  std::string expected_digest;
  if (row.Integer(7) == 0) {
    Require(row.IsNull(12) && row.IsNull(14) && row.Integer(19) == 0 &&
                kind == CellAdmissionSettlementKindDTO::kProvenNoEffect,
            "component_lifecycle_claim_settlement_invalid",
            "unaccepted CCLC claim has transaction effects");
    expected_digest = claim.intent.request_payload_digest.value();
  } else {
    Require(row.Integer(7) == 1 && row.IsNull(12) != row.IsNull(14) &&
                kind == CellAdmissionSettlementKindDTO::kTerminalEffectsSettled,
            "component_lifecycle_claim_settlement_invalid", "CCLC acceptance family is not exact");
    if (!row.IsNull(12)) {
      const auto status = detail::DecodeComponentLifecycleStatus(row.Blob(12));
      Require(
          expected_family == "component_lifecycle" && row.Integer(11) == 1 &&
              TerminalStatusIsQuiescent(status) &&
              status.selected_bring_up.has_value() ==
                  claim.intent.owner_transaction_id.starts_with("selected-lifecycle.") &&
              (status.selected_bring_up
                   ? status.selected_bring_up->offer_digest.value()
                   : detail::ComponentLifecycleSemanticIntentDigest(status.accepted_intent)) ==
                  claim.intent.request_payload_digest.value() &&
              (!status.selected_bring_up ||
               (status.selected_bring_up->selected_claim == claim && original_digest &&
                status.selected_bring_up->original_request_payload_digest == *original_digest)) &&
              status.accepted_intent.program_node_evidence == node,
          "component_lifecycle_claim_settlement_invalid", "lifecycle terminal evidence differs");
      expected_digest = detail::ComponentLifecycleStatusReleasePreconditionDigest(status);
    } else {
      const auto status = detail::DecodeRouteSelectionStatus(row.Blob(14));
      if (recovery) {
        const auto& expected = status.accepted_intent.expected_active_control_configuration;
        Require(expected.presence == ExpectedActiveControlConfigurationPresenceDTO::kPresent &&
                    claim.intent.scope.configurations.front() ==
                        CellAdmissionConfigurationScopeDTO{
                            status.accepted_intent.desired_configuration.control_domain_id,
                            *expected.active_control_configuration_id,
                            *expected.active_control_configuration_generation},
                "component_lifecycle_claim_settlement_invalid",
                "recovery configuration differs from the accepted route predecessor");
      }
      Require(expected_family == "route_selection" && row.Integer(13) == 1 && Terminal(status) &&
                  status.accepted_intent.expected_admission_generation ==
                      claim.intent.expected_admission_generation &&
                  status.accepted_intent.program_node_evidence == node &&
                  detail::RouteSelectionSemanticIntentDigest(status.accepted_intent) ==
                      claim.intent.request_payload_digest.value(),
              "component_lifecycle_claim_settlement_invalid", "route terminal evidence differs");
      expected_digest = detail::RouteSelectionStatusReleasePreconditionDigest(status);
    }
  }
  Require(row.Text(9) == expected_digest && !row.Step(),
          "component_lifecycle_claim_settlement_invalid", "CCLC terminal digest is not exact");
  return {claim.intent.owner_role,
          claim.intent.owner_epoch,
          claim.intent.owner_transaction_id,
          claim.intent.request_id,
          claim.intent.request_payload_digest,
          claim.acquired_admission_generation,
          claim.claim_generation,
          {kind, ContentDigest(expected_digest)}};
}

}  // namespace

Result<void> ValidateCCLCCellAdmissionSettlementReadOnly(
    const std::filesystem::path& database_path, const CellId& cell_id,
    const CellAdmissionClaimDTO& claim, const CellAdmissionClaimReleaseDTO& release) {
  return AsResult<void>([&]() {
    Require(!database_path.empty() && !cell_id.empty() &&
                ValidateCellAdmissionClaimIntent(claim.intent).ok() &&
                claim.claim_generation.nonzero() && claim.acquired_admission_generation.nonzero() &&
                ValidateCellAdmissionClaimRelease(release).ok() &&
                ValidCCLCClaimIntent(claim.intent) &&
                claim.intent.owner_role == CellAdmissionOwnerRoleDTO::kComponentLifecycleOwner &&
                release.owner_role == claim.intent.owner_role &&
                release.owner_epoch == claim.intent.owner_epoch &&
                release.owner_transaction_id == claim.intent.owner_transaction_id &&
                release.request_id == claim.intent.request_id &&
                release.request_payload_digest == claim.intent.request_payload_digest &&
                release.acquired_admission_generation == claim.acquired_admission_generation &&
                release.claim_generation == claim.claim_generation,
            "component_lifecycle_claim_settlement_invalid",
            "lifecycle settlement does not identify one exact held claim");
    sqlite3* raw = nullptr;
    const auto path = database_path.string();
    const int opened = sqlite3_open_v2(
        path.c_str(), &raw,
        SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX | SQLITE_OPEN_NOFOLLOW,
        nullptr);
    if (opened != SQLITE_OK) {
      const auto detail = SqliteDetail(raw, opened);
      if (raw != nullptr) sqlite3_close(raw);
      Fail("component_lifecycle_journal_storage_failure",
           "open lifecycle settlement journal read-only: " + detail);
    }
    std::unique_ptr<sqlite3, decltype(&sqlite3_close)> database(raw,
                                                               sqlite3_close);
    Check(database.get(), sqlite3_busy_timeout(database.get(),
                                               kBusyTimeoutMilliseconds),
          "configure lifecycle settlement read timeout");
    Exec(database.get(), "PRAGMA query_only=ON");
    Statement application(database.get(), "PRAGMA application_id");
    Require(application.Step() &&
                application.Integer(0) == kCCLCJournalSqliteApplicationId &&
                !application.Step(),
            "component_lifecycle_journal_corrupt",
            "lifecycle settlement journal application identity differs");
    Statement metadata(
        database.get(),
        "SELECT schema_id,schema_version,cell_id FROM cclc_journal_metadata "
        "WHERE singleton=1");
    Require(metadata.Step() && metadata.Text(0) == kCCLCJournalSchemaId &&
                metadata.Integer(1) == kCCLCJournalSchemaVersion &&
                metadata.Text(2) == cell_id.value() && !metadata.Step(),
            "component_lifecycle_journal_corrupt",
            "lifecycle settlement journal metadata differs");
    Require(ReadSettledCCLCClaim(database.get(), claim) == release,
            "component_lifecycle_claim_settlement_invalid",
            "CCLC release does not match owner proof");
  });
}

Result<CellAdmissionReconcileValidationDTO> ValidateCCLCCellAdmissionReconciliationReadOnly(
    const std::filesystem::path& database_path, const CellId& cell_id,
    const std::vector<CellAdmissionClaimDTO>& held_claims,
    const std::vector<CellAdmissionCurrentOwnerEpochDTO>& current_owner_epochs,
    const CellAdmissionReconcileDTO& reconcile) {
  return AsResult<CellAdmissionReconcileValidationDTO>([&]() {
    const auto lifecycle_owner = std::find_if(
        reconcile.current_owner_epochs.begin(),
        reconcile.current_owner_epochs.end(), [](const auto& owner) {
          return owner.owner_role ==
                 CellAdmissionOwnerRoleDTO::kComponentLifecycleOwner;
        });
    const auto expected_evidence =
        lifecycle_owner == reconcile.current_owner_epochs.end()
            ? Result<ContentDigest>::Err(
                  ErrorDTO{"invalid_request", "CCLC owner is absent", {}})
            : CellAdmissionOwnerEpochReconciliationEvidenceDigest(
                  lifecycle_owner->owner_role, lifecycle_owner->owner_epoch);
    const bool owners_canonical =
        std::is_sorted(
            reconcile.current_owner_epochs.begin(),
            reconcile.current_owner_epochs.end(),
            [](const auto& left, const auto& right) {
              return std::tie(left.owner_role, left.owner_epoch) <
                     std::tie(right.owner_role, right.owner_epoch);
            }) &&
        std::adjacent_find(
            reconcile.current_owner_epochs.begin(),
            reconcile.current_owner_epochs.end(),
            [](const auto& left, const auto& right) {
              return left.owner_role == right.owner_role;
            }) == reconcile.current_owner_epochs.end();
    const bool claims_canonical =
        std::is_sorted(
            held_claims.begin(), held_claims.end(),
            [](const auto& left, const auto& right) {
              return left.claim_generation < right.claim_generation;
            }) &&
        std::adjacent_find(
            held_claims.begin(), held_claims.end(),
            [](const auto& left, const auto& right) {
              return left.claim_generation == right.claim_generation;
            }) == held_claims.end();
    Require(!database_path.empty() && !cell_id.empty() &&
                held_claims == reconcile.retained_claims &&
                owners_canonical && claims_canonical &&
                std::all_of(
                    reconcile.current_owner_epochs.begin(),
                    reconcile.current_owner_epochs.end(),
                    [](const auto& owner) {
                      return !to_string(owner.owner_role).empty() &&
                             !owner.owner_epoch.empty();
                    }) &&
                lifecycle_owner != reconcile.current_owner_epochs.end() &&
                !lifecycle_owner->owner_epoch.empty() &&
                expected_evidence.ok() &&
                reconcile.reconciliation_evidence_digest ==
                    expected_evidence.value(),
            "component_lifecycle_claim_reconciliation_invalid",
            "lifecycle reconciliation identity or inventory is not exact");
    for (const auto& owner : current_owner_epochs) {
      if (owner.owner_role ==
          CellAdmissionOwnerRoleDTO::kComponentLifecycleOwner) {
        continue;
      }
      Require(std::find(reconcile.current_owner_epochs.begin(),
                        reconcile.current_owner_epochs.end(), owner) !=
                  reconcile.current_owner_epochs.end(),
              "component_lifecycle_claim_reconciliation_unsupported",
              "CCLC reconciliation omitted another current owner");
    }
    for (const auto& owner : reconcile.current_owner_epochs) {
      if (owner.owner_role ==
          CellAdmissionOwnerRoleDTO::kComponentLifecycleOwner) {
        continue;
      }
      Require(std::find(current_owner_epochs.begin(),
                        current_owner_epochs.end(), owner) !=
                  current_owner_epochs.end(),
              "component_lifecycle_claim_reconciliation_unsupported",
              "CCLC evidence cannot install or replace another owner");
    }

    sqlite3* raw = nullptr;
    const auto path = database_path.string();
    const int opened = sqlite3_open_v2(
        path.c_str(), &raw,
        SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX | SQLITE_OPEN_NOFOLLOW,
        nullptr);
    if (opened != SQLITE_OK) {
      const auto detail = SqliteDetail(raw, opened);
      if (raw != nullptr) sqlite3_close(raw);
      Fail("component_lifecycle_journal_storage_failure",
           "open lifecycle reconciliation journal read-only: " + detail);
    }
    std::unique_ptr<sqlite3, decltype(&sqlite3_close)> database(raw,
                                                               sqlite3_close);
    Check(database.get(), sqlite3_busy_timeout(database.get(),
                                               kBusyTimeoutMilliseconds),
          "configure lifecycle reconciliation read timeout");
    Exec(database.get(), "PRAGMA query_only=ON");
    Exec(database.get(), "BEGIN");
    Statement application(database.get(), "PRAGMA application_id");
    Require(application.Step() &&
                application.Integer(0) == kCCLCJournalSqliteApplicationId &&
                !application.Step(),
            "component_lifecycle_journal_corrupt",
            "lifecycle reconciliation journal application identity differs");
    Statement metadata(
        database.get(),
        "SELECT schema_id,schema_version,cell_id FROM cclc_journal_metadata "
        "WHERE singleton=1");
    Require(metadata.Step() && metadata.Text(0) == kCCLCJournalSchemaId &&
                metadata.Integer(1) == kCCLCJournalSchemaVersion &&
                metadata.Text(2) == cell_id.value() && !metadata.Step(),
            "component_lifecycle_journal_corrupt",
            "lifecycle reconciliation journal metadata differs");
    Statement active_owner(
        database.get(),
        "SELECT owner_epoch FROM cclc_owner_epochs WHERE active=1");
    Require(active_owner.Step() &&
                active_owner.Text(0) == lifecycle_owner->owner_epoch.value() &&
                !active_owner.Step(),
            "component_lifecycle_journal_owner_epoch_stale",
            "lifecycle reconciliation successor is not the exact active owner");
    Statement unresolved(
        database.get(),
        "SELECT "
        "(SELECT count(*) FROM cclc_effect_attempts "
        " WHERE completed_attempt_blob IS NULL),"
        "(SELECT count(*) FROM cclc_route_effect_attempts "
        " WHERE completed_attempt_blob IS NULL),"
        "(SELECT count(*) FROM lifecycle_transactions "
        " WHERE terminal=0),"
        "(SELECT count(*) FROM route_selection_transactions "
        " WHERE terminal=0)");
    Require(unresolved.Step() && unresolved.Integer(0) == 0 &&
                unresolved.Integer(1) == 0 && unresolved.Integer(2) == 0 &&
                unresolved.Integer(3) == 0 && !unresolved.Step(),
            "component_lifecycle_claim_reconciliation_unresolved",
            "CCLC durable physical effects are unresolved");

    CellAdmissionReconcileValidationDTO validation{
        reconcile.current_owner_epochs, {}};
    for (const auto& claim : held_claims) {
      const auto current = std::find_if(
          reconcile.current_owner_epochs.begin(),
          reconcile.current_owner_epochs.end(), [&](const auto& owner) {
            return owner.owner_role == claim.intent.owner_role;
          });
      Require(ValidateCellAdmissionClaimIntent(claim.intent).ok() &&
                  claim.claim_generation.nonzero() &&
                  claim.acquired_admission_generation.nonzero() &&
                  claim.accepted_at_ns >= 0 &&
                  current != reconcile.current_owner_epochs.end(),
              "component_lifecycle_claim_reconciliation_invalid",
              "held claim has no exact current owner membership");
      if (claim.intent.owner_role !=
          CellAdmissionOwnerRoleDTO::kComponentLifecycleOwner) {
        Require(current->owner_epoch == claim.intent.owner_epoch,
                "component_lifecycle_claim_reconciliation_unsupported",
                "CCLC evidence cannot replace another durable owner");
        continue;
      }
      if (current->owner_epoch == claim.intent.owner_epoch) {
        continue;
      }
      static_cast<void>(ReadSettledCCLCClaim(database.get(), claim));
      validation.settlements.push_back(
          {claim, current->owner_role, current->owner_epoch,
           CellAdmissionSettlementKindDTO::kReconciled,
           reconcile.reconciliation_evidence_digest});
    }
    Exec(database.get(), "COMMIT");
    return validation;
  });
}

}  // namespace manyforge::core
