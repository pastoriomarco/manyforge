#include <manyforge_core/admission_timing_diagnostic.hpp>
#include <manyforge_core/execution_event_change_signal.hpp>
#include <manyforge_core/io_rule_outcome_codec.hpp>
#include <sqlite3.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <functional>
#include <limits>
#include <manyforge_content_identity/canonical_value.hpp>
#include <manyforge_content_identity/content_identity.hpp>
#include <manyforge_core/resource_slot_table.hpp>
#include <manyforge_core/sqlite_operation_journal.hpp>
#include <manyforge_core/stop_journal_projection_codec.hpp>
#include <manyforge_core/stop_scope_resolver.hpp>
#include <manyforge_core/tcp_selector_codec.hpp>
#include <manyforge_core/types/typed_error.hpp>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace manyforge::core {
namespace {

using content_identity::CanonicalValue;

constexpr int kBusyTimeoutMilliseconds = 250;
constexpr std::uint64_t kMaximumSafeInteger = 9007199254740991ULL;
constexpr std::int64_t kMaximumDatabasePageImageBytes = 8LL * 1024LL * 1024LL * 1024LL;
constexpr std::size_t kMaximumRecordJsonBytes = 64U * 1024U * 1024U;
constexpr std::size_t kMaximumRequestCapacity = 4096U;
// Matches the Kernel's resident Stop terminal history bound.
constexpr std::size_t kMaximumRecoveredStops = 64U;
constexpr std::size_t kMaximumWallTimeBytes = 128U;

constexpr std::string_view kSchema = R"sql(
CREATE TABLE journal_metadata (
  singleton INTEGER PRIMARY KEY CHECK (singleton = 1),
  schema_id TEXT NOT NULL,
  schema_version INTEGER NOT NULL,
  cell_id TEXT NOT NULL,
  history_capacity INTEGER NOT NULL,
  cell_admission_generation_high_water INTEGER NOT NULL,
  cell_claim_generation_high_water INTEGER NOT NULL
) STRICT;
CREATE TABLE owner_epochs (
  epoch_sequence INTEGER PRIMARY KEY AUTOINCREMENT,
  owner_epoch TEXT NOT NULL UNIQUE,
  started_at_ns INTEGER NOT NULL,
  request_capacity INTEGER NOT NULL,
  active INTEGER NOT NULL CHECK (active IN (0, 1)),
  closed_at_ns INTEGER,
  close_sequence INTEGER UNIQUE,
  CHECK ((closed_at_ns IS NULL) = (close_sequence IS NULL))
) STRICT;
CREATE UNIQUE INDEX one_active_owner_epoch
  ON owner_epochs(active) WHERE active = 1;
CREATE TABLE journal_records (
  sequence INTEGER PRIMARY KEY AUTOINCREMENT,
  owner_epoch TEXT NOT NULL,
  record_kind TEXT NOT NULL,
  request_id TEXT,
  stop_id TEXT,
  stop_generation INTEGER,
  operation_id TEXT,
  operation_revision INTEGER,
  occurred_at_ns INTEGER NOT NULL,
  occurred_at_wall TEXT NOT NULL,
  effect_pending INTEGER NOT NULL CHECK (effect_pending IN (0, 1)),
  payload_json TEXT NOT NULL,
  payload_digest TEXT NOT NULL,
  CHECK ((operation_id IS NULL) = (operation_revision IS NULL)),
  CHECK ((stop_id IS NULL) = (stop_generation IS NULL)),
  CHECK ((record_kind IN ('stop_acceptance_projected','stop_terminal_projected')) =
         (stop_id IS NOT NULL)),
  CHECK (record_kind <> 'stop_acceptance_projected' OR
         (request_id IS NOT NULL AND operation_id IS NULL)),
  FOREIGN KEY(owner_epoch) REFERENCES owner_epochs(owner_epoch)
) STRICT;
CREATE UNIQUE INDEX journal_request_identity
  ON journal_records(owner_epoch, request_id)
  WHERE request_id IS NOT NULL AND record_kind <> 'stop_acceptance_projected';
CREATE UNIQUE INDEX journal_stop_acceptance_identity
  ON journal_records(owner_epoch, request_id, stop_id, stop_generation)
  WHERE record_kind = 'stop_acceptance_projected';
CREATE UNIQUE INDEX journal_stop_terminal_identity
  ON journal_records(stop_id, stop_generation)
  WHERE record_kind = 'stop_terminal_projected';
CREATE INDEX journal_operation_sequence
  ON journal_records(operation_id, sequence);
CREATE TABLE stop_projections (
  stop_id TEXT NOT NULL,
  stop_generation INTEGER NOT NULL,
  acceptance_sequence INTEGER NOT NULL UNIQUE,
  accepted_payload TEXT NOT NULL,
  accepted_digest TEXT NOT NULL,
  terminal_sequence INTEGER UNIQUE,
  PRIMARY KEY(stop_id, stop_generation),
  FOREIGN KEY(acceptance_sequence) REFERENCES journal_records(sequence),
  FOREIGN KEY(terminal_sequence) REFERENCES journal_records(sequence)
) STRICT;
CREATE TABLE request_tombstones (
  tombstone_sequence INTEGER PRIMARY KEY AUTOINCREMENT,
  owner_epoch TEXT NOT NULL,
  request_id TEXT NOT NULL,
  record_kind TEXT NOT NULL,
  operation_id TEXT NOT NULL,
  payload_digest TEXT NOT NULL,
  UNIQUE(owner_epoch, request_id)
) STRICT;
CREATE TABLE operation_snapshots (
  operation_id TEXT PRIMARY KEY,
  owner_epoch TEXT NOT NULL,
  operation_revision INTEGER NOT NULL,
  program_binding_id TEXT NOT NULL,
  program_run_id TEXT NOT NULL,
  coordination_policy TEXT NOT NULL,
  state TEXT NOT NULL,
  updated_at_ns INTEGER NOT NULL,
  active_stop_id TEXT,
  active_stop_generation INTEGER,
  runtime_scale_generation INTEGER,
  disposition TEXT NOT NULL,
  snapshot_sequence INTEGER NOT NULL,
  journal_capture_order INTEGER NOT NULL DEFAULT 0 CHECK(journal_capture_order >= 0),
  FOREIGN KEY(owner_epoch) REFERENCES owner_epochs(owner_epoch),
  FOREIGN KEY(snapshot_sequence) REFERENCES journal_records(sequence)
) STRICT;
CREATE INDEX operation_snapshot_epoch
  ON operation_snapshots(owner_epoch, snapshot_sequence);
CREATE TABLE operation_participants (
  operation_id TEXT NOT NULL,
  participant_index INTEGER NOT NULL,
  participant_id TEXT NOT NULL,
  command_id TEXT NOT NULL,
  command_generation INTEGER NOT NULL,
  controllable_component_id TEXT NOT NULL,
  command_target_id TEXT NOT NULL,
  capability_id TEXT NOT NULL,
  control_domain_id TEXT NOT NULL,
  control_domain_revision TEXT NOT NULL,
  control_binding_id TEXT NOT NULL,
  control_binding_revision TEXT NOT NULL,
  capability_provider_binding_id TEXT,
  capability_provider_binding_revision TEXT,
  payload_kind TEXT NOT NULL,
  state TEXT NOT NULL,
  progress REAL NOT NULL,
  terminal_error_code TEXT,
  terminal_error_message TEXT,
  execution_node_instance_id TEXT,
  execution_node_attempt_id TEXT,
  CHECK ((capability_provider_binding_id IS NULL) =
         (capability_provider_binding_revision IS NULL)),
  PRIMARY KEY(operation_id, participant_index),
  FOREIGN KEY(operation_id) REFERENCES operation_snapshots(operation_id)
    ON DELETE CASCADE
) STRICT;
CREATE TABLE operation_resources (
  operation_id TEXT NOT NULL,
  participant_index INTEGER NOT NULL,
  resource_index INTEGER NOT NULL,
  resource_id TEXT NOT NULL,
  PRIMARY KEY(operation_id, participant_index, resource_index),
  UNIQUE(operation_id, participant_index, resource_id),
  FOREIGN KEY(operation_id, participant_index)
    REFERENCES operation_participants(operation_id, participant_index)
    ON DELETE CASCADE
) STRICT;
CREATE TABLE operation_error_details (
  operation_id TEXT NOT NULL,
  participant_index INTEGER NOT NULL,
  detail_key TEXT NOT NULL,
  detail_value TEXT NOT NULL,
  PRIMARY KEY(operation_id, participant_index, detail_key),
  FOREIGN KEY(operation_id, participant_index)
    REFERENCES operation_participants(operation_id, participant_index)
    ON DELETE CASCADE
) STRICT;
CREATE TABLE cell_admission_claims (
  claim_generation INTEGER PRIMARY KEY,
  kernel_owner_epoch TEXT NOT NULL,
  claim_kind TEXT NOT NULL,
  owner_role TEXT NOT NULL,
  owner_epoch TEXT NOT NULL,
  owner_transaction_id TEXT NOT NULL,
  request_id TEXT NOT NULL,
  request_payload_digest TEXT NOT NULL,
  expected_admission_generation INTEGER NOT NULL,
  acquired_admission_generation INTEGER NOT NULL,
  accepted_at_ns INTEGER NOT NULL,
  draft_program_revision TEXT,
  applied_program_revision TEXT,
  offering_owner_role TEXT,
  offering_owner_epoch TEXT,
  offer_transaction_id TEXT,
  offer_digest TEXT,
  parent_program_cycle_claim_generation INTEGER,
  program_node_runner_owner_epoch TEXT,
  program_node_run_id TEXT,
  program_node_instance_id TEXT,
  program_node_attempt_id TEXT,
  program_node_owner_api TEXT,
  release_owner_role TEXT,
  release_owner_epoch TEXT,
  release_owner_transaction_id TEXT,
  release_request_id TEXT,
  release_request_payload_digest TEXT,
  release_acquired_admission_generation INTEGER,
  release_settlement_kind TEXT,
  release_evidence_digest TEXT,
  reconciled_sequence INTEGER,
  UNIQUE(owner_epoch, request_id),
  CHECK ((draft_program_revision IS NULL) = (applied_program_revision IS NULL)),
  CHECK ((offering_owner_role IS NULL) = (offering_owner_epoch IS NULL)),
  CHECK ((offering_owner_role IS NULL) = (offer_transaction_id IS NULL)),
  CHECK ((offering_owner_role IS NULL) = (offer_digest IS NULL)),
  CHECK ((release_owner_role IS NULL) = (release_owner_epoch IS NULL)),
  CHECK ((release_owner_role IS NULL) = (release_settlement_kind IS NULL)),
  CHECK (reconciled_sequence IS NULL OR release_owner_role IS NULL)
) STRICT;
CREATE TABLE cell_admission_request_membership (
  owner_role TEXT NOT NULL,
  owner_epoch TEXT NOT NULL,
  request_id TEXT NOT NULL,
  semantic_intent_digest TEXT NOT NULL,
  claim_generation INTEGER NOT NULL UNIQUE,
  PRIMARY KEY(owner_epoch, request_id)
) STRICT;
CREATE TABLE cell_admission_current_owners (
  owner_role TEXT PRIMARY KEY,
  owner_epoch TEXT NOT NULL,
  established_kernel_owner_epoch TEXT NOT NULL
) STRICT;
CREATE TABLE cell_admission_claim_resources (
  claim_generation INTEGER NOT NULL,
  resource_index INTEGER NOT NULL,
  resource_id TEXT NOT NULL,
  PRIMARY KEY(claim_generation, resource_index),
  UNIQUE(claim_generation, resource_id),
  FOREIGN KEY(claim_generation) REFERENCES cell_admission_claims(claim_generation)
    ON DELETE CASCADE
) STRICT;
CREATE TABLE cell_admission_claim_configurations (
  claim_generation INTEGER NOT NULL,
  configuration_index INTEGER NOT NULL,
  control_domain_id TEXT NOT NULL,
  active_configuration_id TEXT NOT NULL,
  active_configuration_generation INTEGER NOT NULL,
  PRIMARY KEY(claim_generation, configuration_index),
  UNIQUE(claim_generation, control_domain_id, active_configuration_id,
         active_configuration_generation),
  FOREIGN KEY(claim_generation) REFERENCES cell_admission_claims(claim_generation)
    ON DELETE CASCADE
) STRICT;
CREATE TABLE cell_admission_reconciliations (
  reconcile_sequence INTEGER PRIMARY KEY AUTOINCREMENT,
  kernel_owner_epoch TEXT NOT NULL,
  evidence_digest TEXT NOT NULL,
  inventory_digest TEXT NOT NULL,
  current_owners_digest TEXT NOT NULL,
  authorization_digest TEXT NOT NULL,
  resulting_generation INTEGER NOT NULL,
  UNIQUE(kernel_owner_epoch, evidence_digest)
) STRICT;
CREATE TABLE cell_admission_reconcile_settlements (
  reconcile_sequence INTEGER NOT NULL,
  settlement_index INTEGER NOT NULL,
  claim_generation INTEGER NOT NULL UNIQUE,
  predecessor_intent_digest TEXT NOT NULL,
  predecessor_owner_role TEXT NOT NULL,
  predecessor_owner_epoch TEXT NOT NULL,
  successor_owner_role TEXT NOT NULL,
  successor_owner_epoch TEXT NOT NULL,
  settlement_predicate TEXT NOT NULL,
  evidence_digest TEXT NOT NULL,
  PRIMARY KEY(reconcile_sequence, settlement_index),
  FOREIGN KEY(reconcile_sequence)
    REFERENCES cell_admission_reconciliations(reconcile_sequence) ON DELETE CASCADE
) STRICT;
)sql";

class JournalFailure final : public TypedError {
 public:
  JournalFailure(std::string code, std::string message)
      : TypedError(std::move(code), message) {}
};

[[noreturn]] void Fail(std::string code, std::string message) {
  throw JournalFailure(std::move(code), std::move(message));
}

void Require(bool condition, std::string code, std::string message) {
  if (!condition) {
    Fail(std::move(code), std::move(message));
  }
}

[[nodiscard]] std::string ProductionWallTimeText() {
  const auto now = std::chrono::system_clock::now();
  const auto seconds = std::chrono::time_point_cast<std::chrono::seconds>(now);
  const auto nanoseconds =
      std::chrono::duration_cast<std::chrono::nanoseconds>(now - seconds).count();
  const std::time_t raw = std::chrono::system_clock::to_time_t(seconds);
  std::tm value{};
  Require(::gmtime_r(&raw, &value) != nullptr, "operation_journal_clock_failure",
          "operation journal wall clock conversion failed");
  std::array<char, 64> output{};
  const auto count =
      std::snprintf(output.data(), output.size(), "%04d-%02d-%02dT%02d:%02d:%02d.%09lldZ",
                    value.tm_year + 1900, value.tm_mon + 1, value.tm_mday, value.tm_hour,
                    value.tm_min, value.tm_sec, static_cast<long long>(nanoseconds));
  Require(count > 0 && static_cast<std::size_t>(count) < output.size(),
          "operation_journal_clock_failure", "operation journal wall clock formatting failed");
  return std::string(output.data(), static_cast<std::size_t>(count));
}

void ValidateWallTimeText(const std::string& value) {
  Require(!value.empty() && value.size() <= kMaximumWallTimeBytes &&
              content_identity::is_utf8_nfc(value),
          "operation_journal_clock_failure",
          "operation journal wall time must be bounded NFC text");
}

[[nodiscard]] ErrorDTO JournalError(const JournalFailure& error) {
  return ErrorDTO{error.code(), error.what(), {{"reason", "operation_journal_failure"}}};
}

[[nodiscard]] std::string SqliteDetail(sqlite3* database, int result) {
  return database == nullptr ? sqlite3_errstr(result) : sqlite3_errmsg(database);
}

void Check(sqlite3* database, int result, std::string_view operation) {
  if (result != SQLITE_OK) {
    Fail(result == SQLITE_BUSY || result == SQLITE_LOCKED ? "operation_journal_busy"
                                                          : "operation_journal_storage_failure",
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
    Fail(result == SQLITE_BUSY || result == SQLITE_LOCKED ? "operation_journal_busy"
                                                          : "operation_journal_storage_failure",
         "execute SQLite statement: " + detail);
  }
}

// Capture only elapsed SQLite work; output is deferred to the host dispatch boundary.
class AdmissionSqlTiming final {
 public:
  explicit AdmissionSqlTiming(const char* stage) noexcept
      : stage_(stage) {
    if (stage_ != nullptr) started_ = std::chrono::steady_clock::now();
  }
  ~AdmissionSqlTiming() {
    if (stage_ == nullptr) return;
    const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - started_).count();
    if (elapsed >= 50'000'000)
      detail::CaptureAdmissionJournalTiming(stage_, elapsed, rows, changes);
  }
  std::int64_t rows{-1};
  std::int64_t changes{-1};
 private:
  const char* stage_;
  std::chrono::steady_clock::time_point started_;
};

class Statement final {
 public:
  Statement(sqlite3* database, std::string_view sql, const char* timing_stage = nullptr)
      : database_(database), timing_stage_(timing_stage) {
    Check(database_,
          sqlite3_prepare_v3(database_, sql.data(), static_cast<int>(sql.size()),
                             SQLITE_PREPARE_PERSISTENT, &statement_, nullptr),
          "prepare SQLite statement");
  }

  ~Statement() {
    sqlite3_finalize(statement_);
    if (timing_stage_ != nullptr && elapsed_ns_ >= 50'000'000)
      detail::CaptureAdmissionJournalTiming(timing_stage_, elapsed_ns_, rows_, changes_, outcome_);
  }
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

  void Bind(int index, double value) {
    Check(database_, sqlite3_bind_double(statement_, index, value), "bind SQLite real");
  }

  void BindNull(int index) {
    Check(database_, sqlite3_bind_null(statement_, index), "bind SQLite null");
  }

  [[nodiscard]] bool Step() {
    std::chrono::steady_clock::time_point started;
    if (timing_stage_ != nullptr) started = std::chrono::steady_clock::now();
    const auto result = sqlite3_step(statement_);
    if (timing_stage_ != nullptr) {
      elapsed_ns_ += std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now() - started).count();
      if (result == SQLITE_ROW) {
        ++rows_;
      } else if (result == SQLITE_DONE) {
        outcome_ = "completed";
        if (!sqlite3_stmt_readonly(statement_)) changes_ = sqlite3_changes(database_);
      } else {
        outcome_ = "error";
      }
    }
    if (result == SQLITE_ROW) {
      return true;
    }
    if (result == SQLITE_DONE) {
      return false;
    }
    Fail(result == SQLITE_BUSY || result == SQLITE_LOCKED ? "operation_journal_busy"
                                                          : "operation_journal_storage_failure",
         "step SQLite statement: " + SqliteDetail(database_, result));
  }

  [[nodiscard]] std::string Text(int column) const {
    Require(sqlite3_column_type(statement_, column) == SQLITE_TEXT, "operation_journal_corrupt",
            "operation journal text column has an invalid type");
    const auto* bytes = sqlite3_column_text(statement_, column);
    const auto size = sqlite3_column_bytes(statement_, column);
    Require(bytes != nullptr && size >= 0, "operation_journal_corrupt",
            "operation journal text column is unreadable");
    return std::string(reinterpret_cast<const char*>(bytes), static_cast<std::size_t>(size));
  }

  [[nodiscard]] std::int64_t Integer(int column) const {
    Require(sqlite3_column_type(statement_, column) == SQLITE_INTEGER, "operation_journal_corrupt",
            "operation journal integer column has an invalid type");
    return sqlite3_column_int64(statement_, column);
  }

  [[nodiscard]] double Real(int column) const {
    const auto type = sqlite3_column_type(statement_, column);
    Require(type == SQLITE_FLOAT || type == SQLITE_INTEGER, "operation_journal_corrupt",
            "operation journal real column has an invalid type");
    return sqlite3_column_double(statement_, column);
  }

  [[nodiscard]] bool IsNull(int column) const noexcept {
    return sqlite3_column_type(statement_, column) == SQLITE_NULL;
  }

 private:
  sqlite3* database_{};
  sqlite3_stmt* statement_{};
  const char* timing_stage_{};
  std::int64_t elapsed_ns_{};
  std::int64_t rows_{};
  std::int64_t changes_{-1};
  const char* outcome_{"partial"};
};

class ReadSnapshot final {
 public:
  explicit ReadSnapshot(sqlite3* database) : database_(database) { Exec(database_, "BEGIN"); }
  ~ReadSnapshot() { sqlite3_exec(database_, "COMMIT", nullptr, nullptr, nullptr); }
  ReadSnapshot(const ReadSnapshot&) = delete;
  ReadSnapshot& operator=(const ReadSnapshot&) = delete;

 private:
  sqlite3* database_{};
};

class Transaction final {
 public:
  explicit Transaction(sqlite3* database, bool admission_timing = false,
                       bool release_timing = false)
      : database_(database), admission_timing_(admission_timing), release_timing_(release_timing) {
    AdmissionSqlTiming timing(admission_timing_ ? (release_timing_ ? "release_transaction_begin"
                                                           : "acquire_transaction_begin")
                                          : nullptr);
    Exec(database_, "BEGIN IMMEDIATE");
  }

  ~Transaction() {
    if (!committed_) {
      sqlite3_exec(database_, "ROLLBACK", nullptr, nullptr, nullptr);
    }
  }

  void Commit() {
    {
      AdmissionSqlTiming timing(admission_timing_ ? (release_timing_ ? "release_transaction_commit"
                                                             : "acquire_transaction_commit")
                                            : nullptr);
      Exec(database_, "COMMIT");
    }
    committed_ = true;
    // After COMMIT: a woken event-stream reader sees this write.
    execution_event_change_signal().Notify();
  }

 private:
  sqlite3* database_{};
  bool committed_{};
  bool admission_timing_{};
  bool release_timing_{};
};

// Only a failed COMMIT with no live transaction needs durable readback.
// All ordinary writes retain their existing transaction/error behavior.
enum class ConfigurationCommitReadback { kCommitted, kAbsent, kUnknown };
class ConfigurationCommitUnknown final : public std::runtime_error {
 public:
  explicit ConfigurationCommitUnknown(ErrorDTO error)
      : std::runtime_error(error.message), error_(std::move(error)) {}
  const ErrorDTO& error() const noexcept { return error_; }

 private:
  ErrorDTO error_;
};

void CommitConfigurationApply(Transaction& transaction, sqlite3* database,
                              const CellAdmissionClaimDTO& claim, auto&& readback) {
  try {
    transaction.Commit();
  } catch (const JournalFailure& commit_error) {
    // A still-open SQLite transaction has not committed; its destructor rolls
    // back. Never turn this proven pre-effect failure into a sticky closure.
    if (sqlite3_get_autocommit(database) == 0) throw;
    const auto unknown = [&](std::string read_error) {
      return ConfigurationCommitUnknown(
          ErrorDTO{"persistence_outcome_unknown",
                   "configuration Apply journal commit outcome is unknown",
                   {{"reason", "persistence_outcome_unknown"},
                    {"owner_epoch", claim.intent.owner_epoch.value()},
                    {"request_id", claim.intent.request_id.value()},
                    {"owner_transaction_id", claim.intent.owner_transaction_id},
                    {"claim_generation", std::to_string(claim.claim_generation.value())},
                    {"commit_error", std::string(commit_error.what()).substr(0U, 512U)},
                    {"readback_error", std::move(read_error).substr(0U, 512U)}}});
    };
    ConfigurationCommitReadback outcome;
    try {
      ReadSnapshot snapshot(database);
      outcome = readback();
    } catch (const std::exception& read_error) {
      throw unknown(read_error.what());
    }
    if (outcome == ConfigurationCommitReadback::kCommitted) return;
    if (outcome == ConfigurationCommitReadback::kAbsent) throw;
    throw unknown("durable rows do not match either exact committed or exact absent state");
  }
}

[[nodiscard]] std::int64_t ScalarInteger(sqlite3* database, std::string_view sql) {
  Statement statement(database, sql);
  Require(statement.Step(), "operation_journal_corrupt",
          "operation journal scalar query returned no row");
  const auto value = statement.Integer(0);
  Require(!statement.Step(), "operation_journal_corrupt",
          "operation journal scalar query returned multiple rows");
  return value;
}

[[nodiscard]] CanonicalValue Number(std::uint64_t value, std::string_view label) {
  Require(value <= kMaximumSafeInteger, "operation_journal_record_invalid",
          std::string(label) + " exceeds the lossless I-JSON range");
  return CanonicalValue(static_cast<double>(value));
}

[[nodiscard]] CanonicalValue Timestamp(std::int64_t value, std::string_view label) {
  Require(value >= 0, "operation_journal_record_invalid",
          std::string(label) + " must be nonnegative");
  return content_identity::nonnegative_int64_value(value);
}

template <typename StrongId>
[[nodiscard]] CanonicalValue OptionalIdentity(const std::optional<StrongId>& value) {
  return value.has_value() ? CanonicalValue(value->value()) : CanonicalValue(nullptr);
}

[[nodiscard]] CanonicalValue OptionalText(const std::optional<std::string>& value) {
  return value.has_value() ? CanonicalValue(*value) : CanonicalValue(nullptr);
}

[[nodiscard]] CanonicalValue StringArray(const std::vector<ResourceId>& values) {
  CanonicalValue::Array array;
  array.reserve(values.size());
  for (const auto& value : values) {
    array.emplace_back(value.value());
  }
  return CanonicalValue(std::move(array));
}

[[nodiscard]] CanonicalValue ErrorValue(const std::optional<ErrorDTO>& error) {
  if (!error.has_value()) {
    return CanonicalValue(nullptr);
  }
  CanonicalValue::Object details;
  details.reserve(error->details.size());
  for (const auto& [name, value] : error->details) {
    details.emplace_back(name, CanonicalValue(value));
  }
  return CanonicalValue(CanonicalValue::Object{
      {"code", CanonicalValue(error->code)},
      {"details", CanonicalValue(std::move(details))},
      {"message", CanonicalValue(error->message)},
  });
}

[[nodiscard]] CanonicalValue ParticipantFactValue(const ParticipantExecutionFact& fact) {
  Require(std::string_view(to_string(fact.kind)) != "unknown" &&
              std::string_view(to_string(fact.effect)) != "unknown",
          "operation_journal_record_invalid", "participant terminal fact enum is invalid");
  return CanonicalValue(CanonicalValue::Object{{"kind", CanonicalValue(to_string(fact.kind))},
                                               {"effect", CanonicalValue(to_string(fact.effect))},
                                               {"error", ErrorValue(fact.error)}});
}

[[nodiscard]] std::optional<ErrorDTO> ErrorFromValue(const CanonicalValue& value,
                                                     std::string_view context) {
  if (value.is_null()) return std::nullopt;
  const auto* code = value.find_object_member("code");
  const auto* message = value.find_object_member("message");
  const auto* details = value.find_object_member("details");
  Require(value.is_object() && value.as_object().size() == 3U && code && message && details &&
              code->is_string() && message->is_string() && details->is_object(),
          "operation_journal_corrupt", std::string(context) + " error is invalid");
  ErrorDTO result{code->as_string(), message->as_string(), {}};
  for (const auto& [key, detail] : details->as_object()) {
    Require(detail.is_string(), "operation_journal_corrupt",
            std::string(context) + " error detail is invalid");
    result.details.emplace(key, detail.as_string());
  }
  return result;
}

[[nodiscard]] ParticipantExecutionFact ParticipantFactFromValue(const CanonicalValue& value) {
  const auto* kind = value.find_object_member("kind");
  const auto* effect = value.find_object_member("effect");
  const auto* failure = value.find_object_member("error");
  Require(value.is_object() && value.as_object().size() == 3U && kind && effect && failure &&
              kind->is_string() && effect->is_string(),
          "operation_journal_corrupt", "participant terminal fact shape is invalid");
  std::optional<ParticipantExecutionFactKind> parsed_kind;
  for (const auto candidate : {ParticipantExecutionFactKind::kComputedCompletion,
                               ParticipantExecutionFactKind::kLocalFailure,
                               ParticipantExecutionFactKind::kCancelledBeforeDispatch,
                               ParticipantExecutionFactKind::kPreemptedByAddressedStop,
                               ParticipantExecutionFactKind::kPreemptedByReplacement})
    if (kind->as_string() == to_string(candidate)) parsed_kind = candidate;
  std::optional<ParticipantEffectKnowledge> parsed_effect;
  for (const auto candidate :
       {ParticipantEffectKnowledge::kNoEffect, ParticipantEffectKnowledge::kEffectSettled,
        ParticipantEffectKnowledge::kEffectUncertain})
    if (effect->as_string() == to_string(candidate)) parsed_effect = candidate;
  Require(parsed_kind && parsed_effect, "operation_journal_corrupt",
          "participant terminal fact enum is invalid");
  return {*parsed_kind, *parsed_effect, ErrorFromValue(*failure, "participant terminal fact")};
}

[[nodiscard]] CanonicalValue PreemptionValue(const OperationPreemptionDTO& value) {
  return CanonicalValue(CanonicalValue::Object{
      {"kind", CanonicalValue(std::string(to_string(value.kind)))},
      {"by", CanonicalValue(CanonicalValue::Object{
          {"operation_id", CanonicalValue(value.by.operation_id.value())},
          {"node_instance_id", CanonicalValue(value.by.node_instance_id.value())},
          {"node_attempt_id", CanonicalValue(value.by.node_attempt_id)}})}});
}

[[nodiscard]] OperationPreemptionDTO PreemptionFromValue(const CanonicalValue& value) {
  const auto* kind = value.find_object_member("kind");
  const auto* by = value.find_object_member("by");
  Require(value.is_object() && value.as_object().size() == 2U && kind && kind->is_string() &&
              by && by->is_object() && by->as_object().size() == 3U,
          "operation_journal_corrupt", "operation preemption shape is invalid");
  const auto* operation = by->find_object_member("operation_id");
  const auto* node = by->find_object_member("node_instance_id");
  const auto* attempt = by->find_object_member("node_attempt_id");
  Require(operation && operation->is_string() && node && node->is_string() &&
              attempt && attempt->is_string() &&
              (kind->as_string() == "preempted" || kind->as_string() == "superseded_unsent"),
          "operation_journal_corrupt", "operation preemption identity is invalid");
  return {kind->as_string() == "preempted" ? OperationPreemptionKind::kPreempted
                                         : OperationPreemptionKind::kSupersededUnsent,
          {OperationId(operation->as_string()), ProgramNodeInstanceId(node->as_string()),
           attempt->as_string()}};
}

[[nodiscard]] CanonicalValue MotionTakeoverValue(const MotionTakeoverDecisionDTO& value) {
  Require(value.locally_valid(), "operation_journal_record_invalid",
          "motion takeover decision is invalid");
  CanonicalValue certificate(nullptr);
  if (value.stop_certificate) {
    const auto& ref = *value.stop_certificate;
    certificate = CanonicalValue(CanonicalValue::Object{
        {"stop_certificate_id", CanonicalValue(ref.stop_certificate_id.value())},
        {"stop_certificate_revision",
         Number(ref.stop_certificate_revision.value(), "certificate revision")},
        {"stop_id", CanonicalValue(ref.stop_id.value())},
        {"stop_generation", Number(ref.stop_generation.value(), "Stop generation")}});
  }
  return CanonicalValue(CanonicalValue::Object{
      {"incumbent_operation_id", CanonicalValue(value.incumbent_operation_id.value())},
      {"path", CanonicalValue(std::string(to_string(value.path)))},
      {"cause", ErrorValue(value.cause)},
      {"decided_at_ns", Timestamp(value.decided_at_ns, "takeover decision timestamp")},
      {"elapsed_since_acceptance_ns",
       Timestamp(value.elapsed_since_acceptance_ns, "takeover elapsed time")},
      {"preparation_budget_ns",
       Timestamp(value.preparation_budget_ns, "takeover preparation budget")},
      {"planning_budget_ns", Timestamp(value.planning_budget_ns, "takeover planning budget")},
      {"stop_certificate", std::move(certificate)}});
}

[[nodiscard]] MotionTakeoverDecisionDTO MotionTakeoverFromValue(const CanonicalValue& value) {
  const auto* incumbent = value.find_object_member("incumbent_operation_id");
  const auto* path = value.find_object_member("path");
  const auto* cause = value.find_object_member("cause");
  const auto* decided = value.find_object_member("decided_at_ns");
  const auto* elapsed = value.find_object_member("elapsed_since_acceptance_ns");
  const auto* budget = value.find_object_member("preparation_budget_ns");
  const auto* planning = value.find_object_member("planning_budget_ns");
  const auto* certificate = value.find_object_member("stop_certificate");
  Require(value.is_object() && value.as_object().size() == 8U && incumbent &&
              incumbent->is_string() && path && path->is_string() && cause && decided && elapsed &&
              budget && planning && certificate,
          "operation_journal_corrupt", "motion takeover decision shape is invalid");
  std::optional<MotionTakeoverPathDTO> parsed_path;
  for (const auto candidate :
       {MotionTakeoverPathDTO::kMovingReplacement, MotionTakeoverPathDTO::kSpringBackHold,
        MotionTakeoverPathDTO::kOrdinaryContainment})
    if (path->as_string() == to_string(candidate)) parsed_path = candidate;
  Require(parsed_path.has_value(), "operation_journal_corrupt", "motion takeover path is invalid");
  std::optional<StopCertificateRefDTO> ref;
  if (!certificate->is_null()) {
    const auto* id = certificate->find_object_member("stop_certificate_id");
    const auto* revision = certificate->find_object_member("stop_certificate_revision");
    const auto* stop = certificate->find_object_member("stop_id");
    const auto* generation = certificate->find_object_member("stop_generation");
    const auto valid_counter = [](const CanonicalValue* field) {
      return field && field->is_number() && field->as_number() > 0 &&
             field->as_number() <= static_cast<double>(kMaximumSafeInteger) &&
             std::trunc(field->as_number()) == field->as_number();
    };
    Require(certificate->is_object() && certificate->as_object().size() == 4U && id &&
                id->is_string() && stop && stop->is_string() && valid_counter(revision) &&
                valid_counter(generation),
            "operation_journal_corrupt", "motion takeover Stop certificate is invalid");
    ref = StopCertificateRefDTO{
        StopCertificateId(id->as_string()),
        StopCertificateRevision(static_cast<std::uint64_t>(revision->as_number())),
        StopId(stop->as_string()),
        StopGeneration(static_cast<std::uint64_t>(generation->as_number()))};
  }
  auto result = MotionTakeoverDecisionDTO{OperationId(incumbent->as_string()),
                                          *parsed_path,
                                          ErrorFromValue(*cause, "motion takeover cause"),
                                          content_identity::parse_nonnegative_int64(*decided),
                                          content_identity::parse_nonnegative_int64(*elapsed),
                                          content_identity::parse_nonnegative_int64(*budget),
                                          std::move(ref),
                                          content_identity::parse_nonnegative_int64(*planning)};
  Require(result.locally_valid(), "operation_journal_corrupt",
          "motion takeover decision is invalid");
  return result;
}

[[nodiscard]] CanonicalValue RequestContextValue(const PublicRequestContextDTO& context) {
  return CanonicalValue(CanonicalValue::Object{
      {"correlation_id", OptionalText(context.correlation_id)},
      {"request_id", CanonicalValue(context.request_id.value())},
      {"request_owner_epoch", CanonicalValue(context.request_owner_epoch.value())},
      {"source_label", CanonicalValue(context.source_label)},
  });
}

[[nodiscard]] std::string_view RequestTransportName(RequestTransportDTO transport) noexcept {
  switch (transport) {
    case RequestTransportDTO::kRos:
      return "ros";
    case RequestTransportDTO::kHttp:
      return "http";
    case RequestTransportDTO::kInternal:
      return "internal";
  }
  return {};
}

[[nodiscard]] std::string_view RequestAssuranceName(RequestAssuranceDTO assurance) noexcept {
  switch (assurance) {
    case RequestAssuranceDTO::kUnverified:
      return "unverified";
    case RequestAssuranceDTO::kExternallyAuthenticated:
      return "externally_authenticated";
  }
  return {};
}

[[nodiscard]] CanonicalValue RequestAttributionValue(const RequestAttributionDTO& attribution) {
  return CanonicalValue(CanonicalValue::Object{
      {"assurance", CanonicalValue(std::string(RequestAssuranceName(attribution.assurance)))},
      {"principal_id", OptionalText(attribution.principal_id)},
      {"transport", CanonicalValue(std::string(RequestTransportName(attribution.transport)))},
  });
}

[[nodiscard]] std::string_view OperationStateName(OperationStateKind state) noexcept {
  switch (state) {
    case OperationStateKind::kQueued:
      return "queued";
    case OperationStateKind::kResolving:
      return "resolving";
    case OperationStateKind::kPlanning:
      return "planning";
    case OperationStateKind::kValidating:
      return "validating";
    case OperationStateKind::kPrepared:
      return "prepared";
    case OperationStateKind::kDispatching:
      return "dispatching";
    case OperationStateKind::kExecuting:
      return "executing";
    case OperationStateKind::kStopping:
      return "stopping";
    case OperationStateKind::kCompleted:
      return "completed";
    case OperationStateKind::kStopped:
      return "stopped";
    case OperationStateKind::kFailed:
      return "failed";
    case OperationStateKind::kInterrupted:
      return "interrupted";
    case OperationStateKind::kInterventionRequired:
      return "intervention_required";
    case OperationStateKind::kCancelledPreDispatch:
      return "cancelled_pre_dispatch";
  }
  return {};
}

[[nodiscard]] std::optional<OperationStateKind> OperationStateFromName(
    std::string_view value) noexcept {
  constexpr OperationStateKind states[]{
      OperationStateKind::kQueued,
      OperationStateKind::kResolving,
      OperationStateKind::kPlanning,
      OperationStateKind::kValidating,
      OperationStateKind::kPrepared,
      OperationStateKind::kDispatching,
      OperationStateKind::kExecuting,
      OperationStateKind::kStopping,
      OperationStateKind::kCompleted,
      OperationStateKind::kStopped,
      OperationStateKind::kFailed,
      OperationStateKind::kInterrupted,
      OperationStateKind::kInterventionRequired,
      OperationStateKind::kCancelledPreDispatch,
  };
  for (const auto state : states) {
    if (OperationStateName(state) == value) {
      return state;
    }
  }
  return std::nullopt;
}

[[nodiscard]] bool IsTerminal(OperationStateKind state) noexcept {
  return state == OperationStateKind::kCompleted || state == OperationStateKind::kStopped ||
         state == OperationStateKind::kFailed || state == OperationStateKind::kInterrupted ||
         state == OperationStateKind::kInterventionRequired ||
         state == OperationStateKind::kCancelledPreDispatch;
}

[[nodiscard]] std::string_view PayloadKindName(OperationParticipantPayloadKind kind) noexcept {
  return kind == OperationParticipantPayloadKind::kMotion ? "motion" : "capability_command";
}

[[nodiscard]] std::optional<OperationParticipantPayloadKind> PayloadKindFromName(
    std::string_view value) noexcept {
  if (value == "motion") {
    return OperationParticipantPayloadKind::kMotion;
  }
  if (value == "capability_command") {
    return OperationParticipantPayloadKind::kCapabilityCommand;
  }
  return std::nullopt;
}

[[nodiscard]] std::string_view CoordinationName(OperationCoordinationPolicy policy) noexcept {
  return policy == OperationCoordinationPolicy::kIndependent ? "independent" : "coordinated_all";
}

[[nodiscard]] std::optional<OperationCoordinationPolicy> CoordinationFromName(
    std::string_view value) noexcept {
  if (value == "independent") {
    return OperationCoordinationPolicy::kIndependent;
  }
  if (value == "coordinated_all") {
    return OperationCoordinationPolicy::kCoordinatedAll;
  }
  return std::nullopt;
}

[[nodiscard]] std::string_view ControlName(OperationControlKind control) {
  switch (control) {
    case OperationControlKind::kReset:
      return "reset";
    case OperationControlKind::kAcknowledgeCompleted:
      return "acknowledge_completed";
  }
  return {};
}

[[nodiscard]] CanonicalValue PoseValue(const Pose3DTO& pose) {
  return CanonicalValue(CanonicalValue::Object{
      {"orientation_xyzw", CanonicalValue(CanonicalValue::Array{
                               CanonicalValue(pose.orientation.x),
                               CanonicalValue(pose.orientation.y),
                               CanonicalValue(pose.orientation.z),
                               CanonicalValue(pose.orientation.w),
                           })},
      {"position_m", CanonicalValue(CanonicalValue::Array{
                         CanonicalValue(pose.position_m.x_m),
                         CanonicalValue(pose.position_m.y_m),
                         CanonicalValue(pose.position_m.z_m),
                     })},
  });
}

[[nodiscard]] CanonicalValue ReferenceValue(const std::optional<ReferenceDTO>& reference) {
  if (!reference.has_value()) {
    return CanonicalValue(nullptr);
  }
  std::string_view type;
  switch (reference->type) {
    case ReferenceDTO::Type::kFrameRole:
      type = "frame_role";
      break;
    case ReferenceDTO::Type::kComponentBodyRole:
      type = "component_body_role";
      break;
    case ReferenceDTO::Type::kSceneResource:
      type = "scene_resource";
      break;
    case ReferenceDTO::Type::kRuntimeObject:
      type = "runtime_object";
      break;
    case ReferenceDTO::Type::kPose:
      type = "pose";
      break;
    case ReferenceDTO::Type::kCellFrame:
      type = "cell_frame";
      break;
  }
  CanonicalValue::Object value{
      {"id", CanonicalValue(std::string(reference_identity_value(*reference)))},
      {"pose", reference->pose.has_value() ? PoseValue(*reference->pose) : CanonicalValue(nullptr)},
  };
  // Emitted only for robot-qualified frames so unqualified references keep
  // their established canonical identity.
  if (reference->target.has_value()) {
    value.emplace_back(
        "target",
        CanonicalValue(CanonicalValue::Object{
            {"command_target_id", CanonicalValue(reference->target->command_target_id.value())},
            {"controllable_component_id",
             CanonicalValue(reference->target->controllable_component_id.value())},
            {"target_role_id", CanonicalValue(reference->target->target_role_id.value())},
        }));
  }
  value.emplace_back("type", CanonicalValue(std::string(type)));
  return CanonicalValue(std::move(value));
}

[[nodiscard]] CanonicalValue ActionSpanValue(const std::optional<ActionSpanDTO>& span) {
  if (!span.has_value()) {
    return CanonicalValue(nullptr);
  }
  return CanonicalValue(CanonicalValue::Object{
      {"action_source", CanonicalValue(to_string(span->action_source))},
      {"action_span_id", CanonicalValue(span->action_span_id)},
      {"node_attempt_id", CanonicalValue(span->node_attempt_id)},
      {"node_instance_id", CanonicalValue(span->node_instance_id)},
      {"parent_node_attempt_id", CanonicalValue(span->parent_node_attempt_id)},
      {"program_run_id", CanonicalValue(span->program_run_id)},
  });
}

[[nodiscard]] CanonicalValue MotionSpecValue(const MotionSpecDTO& motion) {
  return std::visit(
      [](const auto& value) -> CanonicalValue {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, JointGoalDTO>) {
          CanonicalValue::Array names;
          CanonicalValue::Array positions;
          names.reserve(value.joint_names.size());
          positions.reserve(value.positions_rad.size());
          for (const auto& name : value.joint_names) {
            names.emplace_back(name);
          }
          for (const auto position : value.positions_rad) {
            positions.emplace_back(position);
          }
          return CanonicalValue(CanonicalValue::Object{
              {"joint_names", CanonicalValue(std::move(names))},
              {"kind", CanonicalValue("joint_goal")},
              {"positions_rad", CanonicalValue(std::move(positions))},
          });
        } else if constexpr (std::is_same_v<T, PoseGoalDTO>) {
          return CanonicalValue(CanonicalValue::Object{
              {"goal", PoseValue(value.goal)},
              {"kind", CanonicalValue("pose_goal")},
          });
        } else if constexpr (std::is_same_v<T, NamedTargetDTO>) {
          return CanonicalValue(CanonicalValue::Object{
              {"kind", CanonicalValue("named_target")},
              {"name", CanonicalValue(value.name)},
          });
        } else {
          CanonicalValue::Array waypoints;
          waypoints.reserve(value.waypoints.size());
          for (const auto& waypoint : value.waypoints) {
            waypoints.push_back(PoseValue(waypoint));
          }
          CanonicalValue::Array blend_radii;
          blend_radii.reserve(value.blend_radii_m.size());
          for (const double radius : value.blend_radii_m) blend_radii.emplace_back(radius);
          return CanonicalValue(CanonicalValue::Object{
              {"blend_radii_m", CanonicalValue(std::move(blend_radii))},
              {"fallback_policy",
               CanonicalValue(value.fallback_policy == CartesianFallbackPolicyDTO::kFailClosed
                                  ? "fail_closed"
                                  : "allow_joint_space_fallback")},
              {"kind", CanonicalValue("cartesian_path")},
              {"max_angular_acceleration_rad_s2",
               CanonicalValue(value.max_angular_acceleration_rad_s2)},
              {"max_angular_speed_rad_s", CanonicalValue(value.max_angular_speed_rad_s)},
              {"max_linear_acceleration_mps2", CanonicalValue(value.max_linear_acceleration_mps2)},
              {"max_rotation_step_rad", CanonicalValue(value.max_rotation_step_rad)},
              {"max_translation_step_m", CanonicalValue(value.max_translation_step_m)},
              {"speed_mode", value.speed_mode
                                 ? CanonicalValue(std::string(
                                       cartesian_speed_mode_name(*value.speed_mode)))
                                 : CanonicalValue(nullptr)},
              {"waypoints", CanonicalValue(std::move(waypoints))},
          });
        }
      },
      motion);
}

[[nodiscard]] CanonicalValue CoordinatePositionValue(const CoordinatePositionDTO& position) {
  if (const auto* angular = std::get_if<AngularPositionDTO>(&position)) {
    return CanonicalValue(CanonicalValue::Object{
        {"kind", CanonicalValue("angular")},
        {"value", CanonicalValue(angular->value_rad)},
    });
  }
  return CanonicalValue(CanonicalValue::Object{
      {"kind", CanonicalValue("linear")},
      {"value", CanonicalValue(std::get<LinearPositionDTO>(position).value_m)},
  });
}

[[nodiscard]] CanonicalValue CoordinateEffortValue(
    const std::optional<CoordinateEffortDTO>& effort) {
  if (!effort.has_value()) {
    return CanonicalValue(nullptr);
  }
  if (const auto* angular = std::get_if<AngularEffortDTO>(&*effort)) {
    return CanonicalValue(CanonicalValue::Object{
        {"kind", CanonicalValue("angular")},
        {"value", CanonicalValue(angular->value_nm)},
    });
  }
  return CanonicalValue(CanonicalValue::Object{
      {"kind", CanonicalValue("linear")},
      {"value", CanonicalValue(std::get<LinearEffortDTO>(*effort).value_n)},
  });
}

[[nodiscard]] CanonicalValue OptionalNumber(const std::optional<double>& value) {
  return value.has_value() ? CanonicalValue(*value) : CanonicalValue(nullptr);
}

[[nodiscard]] CanonicalValue MotionPayloadValue(const MotionParticipantIntentDTO& motion) {
  Require(motion.motion_interrupt_scopes.empty(), "operation_journal_payload_unsupported",
          "N4 durable admission does not accept motion-interrupt scopes");
  CanonicalValue::Object dynamics;
  dynamics.reserve(motion.per_joint_dynamics_limits.size());
  for (const auto& [joint_name, limit] : motion.per_joint_dynamics_limits) {
    dynamics.emplace_back(
        joint_name, CanonicalValue(CanonicalValue::Object{
                        {"max_acceleration_rad_s2", OptionalNumber(limit.max_acceleration_rad_s2)},
                        {"max_jerk_rad_s3", OptionalNumber(limit.max_jerk_rad_s3)},
                        {"max_velocity_rad_s", OptionalNumber(limit.max_velocity_rad_s)},
                    }));
  }
  const auto cache_mode = motion.cache_mode == CacheMode::kDefault ? "default" : "disable";
  const auto target_mode =
      motion.target_mode == MotionTargetModeDTO::kAbsolute ? "absolute" : "relative";
  CanonicalValue::Array runtime_objects;
  runtime_objects.reserve(motion.runtime_object_bindings.size());
  for (const auto& binding : motion.runtime_object_bindings) {
    runtime_objects.emplace_back(CanonicalValue::Object{
        {"runtime_object_id", CanonicalValue(binding.runtime_object_id.value())},
        {"scene_resource_id", CanonicalValue(binding.scene_resource_id.value())},
    });
  }
  CanonicalValue::Object value{
      {"base_frame_role_id", CanonicalValue(motion.base_frame_role_id.value())},
      {"cache_mode", CanonicalValue(cache_mode)},
      {"contact_policy_id", OptionalIdentity(motion.contact_policy_id)},
      {"kind", CanonicalValue("motion")},
      {"motion", MotionSpecValue(motion.motion)},
      {"motion_interrupt_scopes", CanonicalValue(CanonicalValue::Array{})},
      {"move_id", CanonicalValue(motion.move_id.value())},
      {"offset_reference", ReferenceValue(motion.offset_reference)},
      {"per_joint_dynamics_limits", CanonicalValue(std::move(dynamics))},
      {"runtime_object_bindings", CanonicalValue(std::move(runtime_objects))},
      {"speed_profile",
       CanonicalValue(CanonicalValue::Object{
           {"acceleration_scale", CanonicalValue(motion.speed_profile.acceleration_scale)},
           {"max_tcp_linear_speed_mps",
            OptionalNumber(motion.speed_profile.max_tcp_linear_speed_mps)},
           {"velocity_scale", CanonicalValue(motion.speed_profile.velocity_scale)},
       })},
      {"target_mode", CanonicalValue(target_mode)},
      {"target_reference", ReferenceValue(motion.target_reference)},
      {"tcp_endpoint", EncodeTcpSelector(motion.tcp_endpoint)},
      {"timeouts",
       [&] {
         CanonicalValue::Object timeouts{
             {"execution_timeout_s", CanonicalValue(motion.timeouts.execution_timeout_s)},
             {"planning_timeout_s", CanonicalValue(motion.timeouts.planning_timeout_s)},
         };
         // Emitted only when set so existing intents keep their digest.
         if (motion.timeouts.zero_speed_hold_timeout_s.has_value()) {
           timeouts.emplace_back("zero_speed_hold_timeout_s",
                                 CanonicalValue(*motion.timeouts.zero_speed_hold_timeout_s));
         }
         return CanonicalValue(std::move(timeouts));
       }()},
      {"tolerances",
       CanonicalValue(CanonicalValue::Object{
           {"joint_position_tol_rad", CanonicalValue(motion.tolerances.joint_position_tol_rad)},
           {"rotation_tol_rad", CanonicalValue(motion.tolerances.rotation_tol_rad)},
           {"translation_tol_m", CanonicalValue(motion.tolerances.translation_tol_m)},
       })},
  };
  // Emitted only for framed absolute goals so existing intents keep their
  // established semantic digest.
  if (motion.goal_frame_reference.has_value()) {
    value.emplace_back("goal_frame_reference", ReferenceValue(motion.goal_frame_reference));
  }
  // Emitted only when the move sets its own clearance, for the same reason.
  if (motion.clearance_m.has_value()) {
    value.emplace_back("clearance_m", CanonicalValue(*motion.clearance_m));
  }
  // Emitted only when the move overrides the Deployment planner block.
  if (motion.planner != PlannerOverrideDTO{}) {
    const auto& planner = motion.planner;
    CanonicalValue::Object fields;
    if (planner.collision_check_step_rad.has_value()) {
      fields.emplace_back("collision_check_step_rad",
                          CanonicalValue(*planner.collision_check_step_rad));
    }
    if (planner.max_nodes.has_value()) {
      fields.emplace_back("max_nodes", CanonicalValue(static_cast<double>(*planner.max_nodes)));
    }
    if (planner.n.has_value()) {
      fields.emplace_back("n", CanonicalValue(static_cast<double>(*planner.n)));
    }
    if (planner.optimize.has_value()) {
      fields.emplace_back("optimize", CanonicalValue(std::string(to_string(*planner.optimize))));
    }
    if (planner.search.has_value()) {
      fields.emplace_back("search", CanonicalValue(std::string(to_string(*planner.search))));
    }
    value.emplace_back("planner", CanonicalValue(std::move(fields)));
  }
  std::sort(value.begin(), value.end(),
            [](const auto& left, const auto& right) { return left.first < right.first; });
  return CanonicalValue(std::move(value));
}

[[nodiscard]] CanonicalValue CapabilityPayloadValue(const CapabilityCommandIntentDTO& command) {
  CanonicalValue command_value(CanonicalValue::Object{
      {"kind", CanonicalValue("gripper_position")},
      {"max_effort", CoordinateEffortValue(command.command_value.max_effort)},
      {"position", CoordinatePositionValue(command.command_value.position)},
  });
  CanonicalValue::Array options;
  options.reserve(command.caller_options.size());
  for (const auto& option : command.caller_options) {
    CanonicalValue::Object row{{"option_id", CanonicalValue(option.option_id.value())}};
    if (const auto* value_s = std::get_if<double>(&option.value)) {
      row.emplace_back("value_s", CanonicalValue(*value_s));
    } else {
      row.emplace_back("value_bool", CanonicalValue(std::get<bool>(option.value)));
    }
    options.emplace_back(std::move(row));
  }
  CanonicalValue::Object result{
      {"caller_options", CanonicalValue(std::move(options))},
      {"command_schema_id", CanonicalValue(command.command_schema_id.value())},
      {"command_schema_revision", CanonicalValue(command.command_schema_revision.value())},
      {"command_value", std::move(command_value)},
      {"kind", CanonicalValue("capability_command")},
  };
  Require(command.completion_timeout_s.has_value(), "operation_journal_record_invalid",
          "capability command has no completion timeout");
  result.emplace_back("completion_timeout_s", CanonicalValue(*command.completion_timeout_s));
  return CanonicalValue(std::move(result));
}

[[nodiscard]] CanonicalValue SceneRevisionValue(const std::optional<SceneRevision>& scene) {
  if (!scene.has_value()) {
    return CanonicalValue(nullptr);
  }
  return CanonicalValue(CanonicalValue::Object{
      {"content_hash", CanonicalValue(scene->content_hash.value())},
      {"revision_id", Number(scene->revision_id, "scene revision")},
      {"scene_id", CanonicalValue(scene->scene_id.value())},
  });
}

void AddExecutionScope(CanonicalValue::Object& row,
                       const std::optional<ProgramNodeExecutionScopeDTO>& scope) {
  if (!scope.has_value()) return;
  Require(valid_program_node_execution_scope(*scope), "operation_journal_record_invalid",
          "operation node execution scope is invalid");
  row.emplace_back("execution_scope",
                   CanonicalValue(CanonicalValue::Object{
                       {"node_attempt_id", CanonicalValue(scope->node_attempt_id)},
                       {"node_instance_id", CanonicalValue(scope->node_instance_id.value())}}));
  std::sort(row.begin(), row.end(),
            [](const auto& left, const auto& right) { return left.first < right.first; });
}

[[nodiscard]] CanonicalValue OperationIntentValue(const OperationIntentDTO& intent) {
  CanonicalValue::Array participants;
  participants.reserve(intent.participants.size());
  for (const auto& participant : intent.participants) {
    CanonicalValue::Object row{
        {"action_span", ActionSpanValue(participant.action_span)},
        {"component_role_id", CanonicalValue(participant.component_role_id.value())},
        {"payload",
         std::holds_alternative<MotionParticipantIntentDTO>(participant.payload)
             ? MotionPayloadValue(std::get<MotionParticipantIntentDTO>(participant.payload))
             : CapabilityPayloadValue(std::get<CapabilityCommandIntentDTO>(participant.payload))},
        {"required_capability_id", CanonicalValue(participant.required_capability_id.value())},
        {"target_role_id", CanonicalValue(participant.target_role_id.value())},
    };
    AddExecutionScope(row, participant.execution_scope);
    participants.emplace_back(std::move(row));
  }
  CanonicalValue::Object result{
      {"coordination_policy",
       CanonicalValue(std::string(CoordinationName(intent.coordination_policy)))},
      {"expected_graph_revision", CanonicalValue(intent.expected_graph_revision.value())},
      {"expected_retained_bundle_id", CanonicalValue(intent.expected_retained_bundle_id.value())},
      {"expected_scene_revision", SceneRevisionValue(intent.expected_scene_revision)},
      {"participants", CanonicalValue(std::move(participants))},
      {"program_binding_id", CanonicalValue(intent.program_binding_id.value())},
      {"program_id", CanonicalValue(intent.program_id.value())},
      {"program_revision", CanonicalValue(intent.program_revision.value())},
      {"program_run_id", CanonicalValue(intent.program_run_id.value())},
      {"request_attribution", RequestAttributionValue(intent.attribution)},
      {"request_context", RequestContextValue(intent.request_context)},
  };
  if (intent.manual_handover) {
    const auto& context = *intent.manual_handover;
    result.emplace_back(
        "manual_handover",
        CanonicalValue(CanonicalValue::Object{
            {"claim_generation", Number(context.claim_generation.value(), "manual parent claim")},
            {"program_run_id", CanonicalValue(context.program_run_id.value())},
            {"program_runner_owner_epoch",
             CanonicalValue(context.program_runner_owner_epoch.value())},
            {"selection_revision", Number(context.selection_revision, "manual hold revision")}}));
    std::sort(result.begin(), result.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
  }
  return CanonicalValue(std::move(result));
}

[[nodiscard]] CanonicalValue OperationAcceptedValue(const OperationAcceptedDTO& accepted) {
  CanonicalValue::Array assignments;
  assignments.reserve(accepted.participant_assignments.size());
  for (const auto& assignment : accepted.participant_assignments) {
    assignments.emplace_back(CanonicalValue::Object{
        {"input_index", Number(assignment.input_index, "participant index")},
        {"participant_id", CanonicalValue(assignment.participant_id.value())},
    });
  }
  return CanonicalValue(CanonicalValue::Object{
      {"accepted_at_ns", Timestamp(accepted.accepted_at_ns, "operation acceptance timestamp")},
      {"operation_id", CanonicalValue(accepted.operation_id.value())},
      {"operation_revision", Number(accepted.operation_revision.value(), "operation revision")},
      {"participant_assignments", CanonicalValue(std::move(assignments))},
      {"replay_status", CanonicalValue(std::string(to_string(accepted.replay_status)))},
      {"request_id", CanonicalValue(accepted.request_id.value())},
  });
}

[[nodiscard]] CanonicalValue OperationStateValue(const OperationStateDTO& state) {
  Require(!has_program_execution_scope(state) || state.journal_capture_order > 0U,
          "operation_journal_record_invalid",
          "captured Program work requires an ordered journal snapshot");
  CanonicalValue::Array participants;
  participants.reserve(state.participants.size());
  for (const auto& participant : state.participants) {
    const auto& controller = participant.binding_identity.controller;
    const auto* provider = participant.binding_identity.capability_provider();
    CanonicalValue::Object row{
        {"capability_id", CanonicalValue(participant.capability_id.value())},
        {"capability_provider_binding_id",
         provider ? CanonicalValue(provider->capability_provider_binding_id.value())
                  : CanonicalValue(nullptr)},
        {"capability_provider_binding_revision",
         provider ? CanonicalValue(provider->capability_provider_binding_revision.value())
                  : CanonicalValue(nullptr)},
        {"command_target_id", CanonicalValue(participant.command_target_id.value())},
        {"control_binding_id", CanonicalValue(controller.control_binding_id.value())},
        {"control_binding_revision", CanonicalValue(controller.control_binding_revision.value())},
        {"control_domain_id",
         CanonicalValue(participant.binding_identity.control_domain_id.value())},
        {"control_domain_revision",
         CanonicalValue(participant.binding_identity.control_domain_revision.value())},
        {"controllable_component_id",
         CanonicalValue(participant.controllable_component_id.value())},
        {"command_generation",
         Number(participant.command_generation.value(), "command generation")},
        {"command_id", CanonicalValue(participant.command_id.value())},
        {"participant_id", CanonicalValue(participant.participant_id.value())},
        {"payload_kind", CanonicalValue(std::string(PayloadKindName(participant.payload_kind)))},
        {"progress", CanonicalValue(participant.progress)},
        {"resources", StringArray(participant.resources)},
        {"state", CanonicalValue(std::string(OperationStateName(participant.state)))},
        {"terminal_error", ErrorValue(participant.terminal_error)},
    };
    AddExecutionScope(row, participant.execution_scope);
    if (participant.terminal_fact)
      row.emplace_back("terminal_fact", ParticipantFactValue(*participant.terminal_fact));
    if (!participant.io_rule_outcomes.empty()) {
      const auto qualified = ValidateIoRuleOutcomeParticipant(
          state.operation_id, state.operation_revision, participant);
      Require(qualified.ok(), "operation_journal_record_invalid",
              "I/O rule registration relabels its containing participant");
      const auto encoded = EncodeIoRuleOutcomes(participant.io_rule_outcomes);
      Require(encoded.ok(), "operation_journal_record_invalid", "I/O rule evidence is invalid");
      row.emplace_back("io_rule_outcomes", content_identity::parse_canonical_json(encoded.value()));
    }
    participants.emplace_back(std::move(row));
  }
  CanonicalValue::Object result{
      {"active_stop_generation",
       state.active_stop_generation.has_value()
           ? Number(state.active_stop_generation->value(), "active Stop generation")
           : CanonicalValue(nullptr)},
      {"active_stop_id", OptionalIdentity(state.active_stop_id)},
      {"coordination_policy",
       CanonicalValue(std::string(CoordinationName(state.coordination_policy)))},
      {"operation_id", CanonicalValue(state.operation_id.value())},
      {"operation_revision", Number(state.operation_revision.value(), "operation revision")},
      {"participants", CanonicalValue(std::move(participants))},
      {"program_binding_id", CanonicalValue(state.program_binding_id.value())},
      {"program_run_id", CanonicalValue(state.program_run_id.value())},
      {"runtime_scale_generation",
       state.runtime_scale_generation.has_value()
           ? Number(state.runtime_scale_generation->value(), "runtime scale generation")
           : CanonicalValue(nullptr)},
      {"state", CanonicalValue(std::string(OperationStateName(state.state)))},
      {"updated_at_ns", Timestamp(state.updated_at_ns, "operation update timestamp")},
  };
  if (state.preemption) result.emplace_back("preemption", PreemptionValue(*state.preemption));
  if (state.motion_takeover)
    result.emplace_back("motion_takeover", MotionTakeoverValue(*state.motion_takeover));
  // Preserve legacy payload bytes on read/migration. Fresh Kernel captures
  // carry this private ordering fact; it is not part of public control DTOs.
  if (state.journal_capture_order != 0U) {
    result.emplace_back("journal_capture_order",
                        CanonicalValue(std::to_string(state.journal_capture_order)));
  }
  return CanonicalValue(std::move(result));
}

[[nodiscard]] CanonicalValue PreparedMotionDiagnosticsValue(
    const OperationStateDTO& state,
    const std::span<const PreparedMotionDiagnosticsDTO> diagnostics) {
  CanonicalValue::Array rows;
  rows.reserve(diagnostics.size());
  std::set<OperationParticipantId> participant_ids;
  for (const auto& diagnostic : diagnostics) {
    const auto state_row = std::find_if(
        state.participants.begin(), state.participants.end(), [&](const auto& candidate) {
          return candidate.participant_id == diagnostic.participant_id;
        });
    const auto& metrics = diagnostic.trajectory_metrics;
    const bool acceleration_shape_valid =
        metrics.peak_abs_joint_acceleration_rad_s2.empty() ||
        metrics.peak_abs_joint_acceleration_rad_s2.size() == diagnostic.joint_names.size();
    const auto finite_nonnegative = [](const double value) {
      return std::isfinite(value) && value >= 0.0;
    };
    Require(state_row != state.participants.end() &&
                state_row->payload_kind == OperationParticipantPayloadKind::kMotion &&
                state_row->state == OperationStateKind::kPrepared &&
                participant_ids.insert(diagnostic.participant_id).second &&
                !diagnostic.prepared_contract_digest.empty() && !diagnostic.move_id.empty() &&
                !diagnostic.joint_names.empty() &&
                diagnostic.joint_names.size() == state_row->resources.size() &&
                diagnostic.joint_names.size() == metrics.peak_abs_joint_velocity_rad_s.size() &&
                acceleration_shape_valid && finite_nonnegative(metrics.trajectory_duration_s) &&
                std::all_of(metrics.peak_abs_joint_velocity_rad_s.begin(),
                            metrics.peak_abs_joint_velocity_rad_s.end(), finite_nonnegative) &&
                std::all_of(metrics.peak_abs_joint_acceleration_rad_s2.begin(),
                            metrics.peak_abs_joint_acceleration_rad_s2.end(), finite_nonnegative) &&
                (!metrics.peak_tcp_linear_speed_mps.has_value() ||
                 finite_nonnegative(*metrics.peak_tcp_linear_speed_mps)) &&
                (!metrics.cartesian_fidelity_metrics.has_value() ||
                 valid_cartesian_fidelity_metrics(*metrics.cartesian_fidelity_metrics)),
            "operation_journal_record_invalid",
            "prepared-motion diagnostics do not match the exact prepared participant");
    std::set<std::string> joint_names;
    Require(std::all_of(
                diagnostic.joint_names.begin(), diagnostic.joint_names.end(),
                [&](const auto& name) { return !name.empty() && joint_names.insert(name).second; }),
            "operation_journal_record_invalid",
            "prepared-motion diagnostics contain invalid joint names");

    Require(std::isfinite(diagnostic.deployment_maximum_tcp_linear_speed_mps) &&
                diagnostic.deployment_maximum_tcp_linear_speed_mps > 0.0 &&
                std::isfinite(diagnostic.admitted_tcp_linear_speed_mps) &&
                diagnostic.admitted_tcp_linear_speed_mps > 0.0 &&
                diagnostic.admitted_tcp_linear_speed_mps <=
                    diagnostic.deployment_maximum_tcp_linear_speed_mps &&
                (!diagnostic.requested_tcp_linear_speed_mps.has_value() ||
                 (std::isfinite(*diagnostic.requested_tcp_linear_speed_mps) &&
                  *diagnostic.requested_tcp_linear_speed_mps > 0.0)) &&
                diagnostic.admitted_tcp_linear_speed_mps ==
                    std::min(diagnostic.requested_tcp_linear_speed_mps.value_or(
                                 diagnostic.deployment_maximum_tcp_linear_speed_mps),
                             diagnostic.deployment_maximum_tcp_linear_speed_mps),
            "operation_journal_record_invalid",
            "prepared-motion diagnostics contain invalid speed provenance");

    CanonicalValue::Array names;
    names.reserve(diagnostic.joint_names.size());
    for (const auto& name : diagnostic.joint_names) {
      names.emplace_back(name);
    }
    const auto numeric_array = [](const std::vector<double>& values) {
      CanonicalValue::Array result;
      result.reserve(values.size());
      for (const auto value : values) {
        result.emplace_back(value);
      }
      return CanonicalValue(std::move(result));
    };
    const auto fidelity_value = [](const auto& value) {
      return CanonicalValue(CanonicalValue::Object{
          {"certified_max_rotation_error_rad",
           CanonicalValue(value->certified_max_rotation_error_rad)},
          {"certified_max_translation_error_m",
           CanonicalValue(value->certified_max_translation_error_m)},
          {"measured_max_rotation_error_rad",
           CanonicalValue(value->measured_max_rotation_error_rad)},
          {"measured_max_translation_error_m",
           CanonicalValue(value->measured_max_translation_error_m)},
          {"requested_rotation_tolerance_rad",
           CanonicalValue(value->requested_rotation_tolerance_rad)},
          {"requested_translation_tolerance_m",
           CanonicalValue(value->requested_translation_tolerance_m)},
          {"unsampled_rotation_bound_rad", CanonicalValue(value->unsampled_rotation_bound_rad)},
          {"unsampled_translation_bound_m", CanonicalValue(value->unsampled_translation_bound_m)},
      });
    };
    CanonicalValue::Object metric_fields{
        {"peak_abs_joint_acceleration_rad_s2",
         numeric_array(metrics.peak_abs_joint_acceleration_rad_s2)},
        {"peak_abs_joint_velocity_rad_s", numeric_array(metrics.peak_abs_joint_velocity_rad_s)},
        {"peak_tcp_linear_speed_mps", metrics.peak_tcp_linear_speed_mps.has_value()
                                          ? CanonicalValue(*metrics.peak_tcp_linear_speed_mps)
                                          : CanonicalValue(nullptr)},
        {"trajectory_duration_s", CanonicalValue(metrics.trajectory_duration_s)}};
    if (metrics.cartesian_fidelity_metrics) {
      metric_fields.emplace_back("cartesian_fidelity_metrics",
                                 fidelity_value(metrics.cartesian_fidelity_metrics));
    }
    if (metrics.peak_tcp_linear_speed_frame) {
      metric_fields.emplace_back("peak_tcp_linear_speed_frame",
                                 CanonicalValue(*metrics.peak_tcp_linear_speed_frame));
    }
    CanonicalValue::Object row{
        {"admitted_tcp_linear_speed_mps",
         CanonicalValue(diagnostic.admitted_tcp_linear_speed_mps)},
        {"deployment_maximum_tcp_linear_speed_mps",
         CanonicalValue(diagnostic.deployment_maximum_tcp_linear_speed_mps)},
        {"joint_names", CanonicalValue(std::move(names))},
        {"move_id", CanonicalValue(diagnostic.move_id.value())},
        {"participant_id", CanonicalValue(diagnostic.participant_id.value())},
        {"prepared_contract_digest", CanonicalValue(diagnostic.prepared_contract_digest.value())},
        {"requested_tcp_linear_speed_mps",
         diagnostic.requested_tcp_linear_speed_mps.has_value()
             ? CanonicalValue(*diagnostic.requested_tcp_linear_speed_mps)
             : CanonicalValue(nullptr)},
        {"trajectory_metrics", CanonicalValue(std::move(metric_fields))}};
    if (!diagnostic.speed_check_frame_role_ids.empty()) {
      CanonicalValue::Array roles;
      for (const auto& role : diagnostic.speed_check_frame_role_ids) roles.emplace_back(role);
      row.emplace_back("speed_check_frame_role_ids", CanonicalValue(std::move(roles)));
    }
    rows.emplace_back(std::move(row));
  }
  return CanonicalValue(std::move(rows));
}

[[nodiscard]] CanonicalValue ControlRequestValue(const OperationControlRequestDTO& request) {
  CanonicalValue::Object row{
      {"expected_operation_revision",
       Number(request.expected_operation_revision.value(), "expected operation revision")},
      {"operation_id", CanonicalValue(request.operation_id.value())},
      {"reason", OptionalText(request.reason)},
      {"request_attribution", RequestAttributionValue(request.attribution)},
      {"request_context", RequestContextValue(request.request_context)},
  };
  AddExecutionScope(row, request.execution_scope);
  return CanonicalValue(std::move(row));
}

[[nodiscard]] CanonicalValue ControlAcceptedValue(const OperationControlAcceptedDTO& accepted) {
  return CanonicalValue(CanonicalValue::Object{
      {"accepted_against_revision",
       Number(accepted.accepted_against_revision.value(), "accepted operation revision")},
      {"accepted_at_ns", Timestamp(accepted.accepted_at_ns, "control acceptance timestamp")},
      {"control", CanonicalValue(std::string(ControlName(accepted.control)))},
      {"operation_id", CanonicalValue(accepted.operation_id.value())},
      {"operation_revision_after_acceptance",
       Number(accepted.operation_revision_after_acceptance.value(),
              "operation revision after control")},
      {"replay_status", CanonicalValue(std::string(to_string(accepted.replay_status)))},
      {"request_id", CanonicalValue(accepted.request_id.value())},
  });
}

[[nodiscard]] CanonicalValue StopIntentValue(const StopIntentDTO& intent) {
  CanonicalValue selector = std::visit(
      [](const auto& targets) -> CanonicalValue {
        using Targets = std::decay_t<decltype(targets)>;
        CanonicalValue::Array rows;
        rows.reserve(targets.size());
        for (const auto& target : targets) {
          if constexpr (std::is_same_v<Targets, std::vector<StopOperationTargetDTO>>) {
            rows.emplace_back(CanonicalValue::Object{
                {"expected_operation_revision", Number(target.expected_operation_revision.value(),
                                                       "Stop expected operation revision")},
                {"expected_stop_generation",
                 Number(target.expected_stop_generation.value(), "Stop expected generation")},
                {"operation_id", CanonicalValue(target.operation_id.value())},
            });
          } else if constexpr (std::is_same_v<Targets, std::vector<StopComponentTargetDTO>>) {
            rows.emplace_back(CanonicalValue::Object{
                {"controllable_component_id",
                 CanonicalValue(target.controllable_component_id.value())},
                {"expected_component_lifecycle_generation",
                 Number(target.expected_component_lifecycle_generation.value(),
                        "Stop expected component lifecycle generation")},
                {"expected_graph_revision", CanonicalValue(target.expected_graph_revision.value())},
            });
          } else if constexpr (std::is_same_v<Targets, std::vector<StopResourceTargetDTO>>) {
            rows.emplace_back(CanonicalValue::Object{
                {"expected_graph_revision", CanonicalValue(target.expected_graph_revision.value())},
                {"expected_operation_resource_generation",
                 Number(target.expected_operation_resource_generation,
                        "Stop expected operation-resource generation")},
                {"expected_stop_generation",
                 Number(target.expected_stop_generation.value(), "Stop expected generation")},
                {"resource_id", CanonicalValue(target.resource_id.value())},
            });
          } else {
            rows.emplace_back(CanonicalValue::Object{
                {"program_run_id", CanonicalValue(target.program_run_id.value())},
            });
          }
        }
        const char* kind = nullptr;
        if constexpr (std::is_same_v<Targets, std::vector<StopOperationTargetDTO>>) {
          kind = "operations";
        } else if constexpr (std::is_same_v<Targets, std::vector<StopComponentTargetDTO>>) {
          kind = "components";
        } else if constexpr (std::is_same_v<Targets, std::vector<StopResourceTargetDTO>>) {
          kind = "resources";
        } else {
          kind = "program_runs";
        }
        return CanonicalValue(CanonicalValue::Object{
            {kind, CanonicalValue(std::move(rows))},
            {"selector_kind", CanonicalValue(kind)},
        });
      },
      intent.selector);
  return CanonicalValue(CanonicalValue::Object{
      {"reason", OptionalText(intent.reason)},
      {"request_attribution", RequestAttributionValue(intent.attribution)},
      {"request_context", RequestContextValue(intent.request_context)},
      {"selector", std::move(selector)},
  });
}

[[nodiscard]] CanonicalValue FullStopIntentValue(const FullStopIntentDTO& intent) {
  return CanonicalValue(CanonicalValue::Object{
      {"expected_stop_generation",
       Number(intent.expected_stop_generation.value(), "Full Stop expected generation")},
      {"request_attribution", RequestAttributionValue(intent.attribution)},
      {"request_context", RequestContextValue(intent.request_context)},
  });
}

[[nodiscard]] CanonicalValue ProjectedStopIntentValue(
    const EmergencyStopProjectedIntentDTO& intent) {
  return std::visit(
      [](const auto& selected) -> CanonicalValue {
        using Intent = std::decay_t<decltype(selected)>;
        if constexpr (std::is_same_v<Intent, std::monostate>) {
          return CanonicalValue(CanonicalValue::Object{
              {"intent_kind", CanonicalValue("malformed_ingress")},
              {"value", CanonicalValue(nullptr)},
          });
        } else if constexpr (std::is_same_v<Intent, StopIntentDTO>) {
          return CanonicalValue(CanonicalValue::Object{
              {"intent_kind", CanonicalValue("ordinary_stop")},
              {"value", StopIntentValue(selected)},
          });
        } else {
          return CanonicalValue(CanonicalValue::Object{
              {"intent_kind", CanonicalValue("full_stop")},
              {"value", FullStopIntentValue(selected)},
          });
        }
      },
      intent);
}

[[nodiscard]] CanonicalValue StopDiagnosticTargetValue(const StopScopeDiagnosticTargetDTO& target) {
  return std::visit(
      [](const auto& selected) -> CanonicalValue {
        using Target = std::decay_t<decltype(selected)>;
        if constexpr (std::is_same_v<Target, std::monostate>) {
          return CanonicalValue(CanonicalValue::Object{
              {"kind", CanonicalValue("cell")},
              {"value", CanonicalValue(nullptr)},
          });
        } else if constexpr (std::is_same_v<Target, OperationId>) {
          return CanonicalValue(CanonicalValue::Object{
              {"kind", CanonicalValue("operation_id")},
              {"value", CanonicalValue(selected.value())},
          });
        } else if constexpr (std::is_same_v<Target, ControllableComponentId>) {
          return CanonicalValue(CanonicalValue::Object{
              {"kind", CanonicalValue("controllable_component_id")},
              {"value", CanonicalValue(selected.value())},
          });
        } else {
          return CanonicalValue(CanonicalValue::Object{
              {"kind", CanonicalValue("resource_id")},
              {"value", CanonicalValue(selected.value())},
          });
        }
      },
      target);
}

[[nodiscard]] CanonicalValue StopDiagnosticValue(const StopScopeStaleValueDTO& value) {
  return std::visit(
      [](const auto& selected) -> CanonicalValue {
        using Value = std::decay_t<decltype(selected)>;
        if constexpr (std::is_same_v<Value, std::uint64_t>) {
          return CanonicalValue(CanonicalValue::Object{
              {"kind", CanonicalValue("unsigned_integer")},
              {"value", Number(selected, "Stop diagnostic unsigned integer")},
          });
        } else if constexpr (std::is_same_v<Value, RequestOwnerEpoch>) {
          return CanonicalValue(CanonicalValue::Object{
              {"kind", CanonicalValue("request_owner_epoch")},
              {"value", CanonicalValue(selected.value())},
          });
        } else if constexpr (std::is_same_v<Value, GraphRevision>) {
          return CanonicalValue(CanonicalValue::Object{
              {"kind", CanonicalValue("graph_revision")},
              {"value", CanonicalValue(selected.value())},
          });
        } else if constexpr (std::is_same_v<Value, OperationRevision>) {
          return CanonicalValue(CanonicalValue::Object{
              {"kind", CanonicalValue("operation_revision")},
              {"value", Number(selected.value(), "Stop diagnostic operation revision")},
          });
        } else if constexpr (std::is_same_v<Value, ComponentLifecycleGeneration>) {
          return CanonicalValue(CanonicalValue::Object{
              {"kind", CanonicalValue("component_lifecycle_generation")},
              {"value", Number(selected.value(), "Stop diagnostic lifecycle generation")},
          });
        } else {
          return CanonicalValue(CanonicalValue::Object{
              {"kind", CanonicalValue("stop_generation")},
              {"value", Number(selected.value(), "Stop diagnostic generation")},
          });
        }
      },
      value);
}

[[nodiscard]] CanonicalValue StopAcceptedValue(const StopAcceptedDTO& accepted) {
  CanonicalValue::Array stale_diagnostics;
  stale_diagnostics.reserve(accepted.stale_diagnostics.size());
  for (const auto& diagnostic : accepted.stale_diagnostics) {
    stale_diagnostics.emplace_back(CanonicalValue::Object{
        {"current", StopDiagnosticValue(diagnostic.current)},
        {"expected", StopDiagnosticValue(diagnostic.expected)},
        {"field", CanonicalValue(std::string(to_string(diagnostic.field)))},
        {"target", StopDiagnosticTargetValue(diagnostic.target)},
    });
  }

  CanonicalValue::Array predecessors;
  predecessors.reserve(accepted.joined_predecessor_stops.size());
  for (const auto& predecessor : accepted.joined_predecessor_stops) {
    predecessors.emplace_back(CanonicalValue::Object{
        {"captured_scope_digest", CanonicalValue(predecessor.captured_scope_digest.value())},
        {"predecessor_stop_generation",
         Number(predecessor.predecessor_stop_generation.value(), "predecessor Stop generation")},
        {"predecessor_stop_id", CanonicalValue(predecessor.predecessor_stop_id.value())},
    });
  }

  CanonicalValue::Array affected_operations;
  affected_operations.reserve(accepted.affected_operation_revisions.size());
  for (const auto& operation : accepted.affected_operation_revisions) {
    affected_operations.emplace_back(CanonicalValue::Object{
        {"operation_id", CanonicalValue(operation.operation_id.value())},
        {"operation_revision_after_acceptance",
         Number(operation.operation_revision_after_acceptance.value(),
                "Stop affected operation revision")},
    });
  }

  return CanonicalValue(CanonicalValue::Object{
      {"accepted_at_ns", Timestamp(accepted.accepted_at_ns, "Stop accepted timestamp")},
      {"affected_operation_revisions", CanonicalValue(std::move(affected_operations))},
      {"captured_scope_digest", CanonicalValue(accepted.captured_scope_digest.value())},
      {"joined_predecessor_stops", CanonicalValue(std::move(predecessors))},
      {"replay_status", CanonicalValue(std::string(to_string(accepted.replay_status)))},
      {"request_id", CanonicalValue(accepted.request_id.value())},
      {"requested_selector_digest", CanonicalValue(accepted.requested_selector_digest.value())},
      {"resolution_disposition",
       CanonicalValue(std::string(to_string(accepted.resolution_disposition)))},
      {"resolution_reason", CanonicalValue(std::string(to_string(accepted.resolution_reason)))},
      {"stale_diagnostics", CanonicalValue(std::move(stale_diagnostics))},
      {"stop_generation", Number(accepted.stop_generation.value(), "accepted Stop generation")},
      {"stop_id", CanonicalValue(accepted.stop_id.value())},
  });
}

[[nodiscard]] std::string_view EmergencyStopEntryStateName(
    EmergencyStopEntryStateDTO state) noexcept {
  switch (state) {
    case EmergencyStopEntryStateDTO::kActive:
      return "active";
    case EmergencyStopEntryStateDTO::kTerminal:
      return "terminal";
  }
  return {};
}

[[nodiscard]] std::string_view EmergencyStopAcceptancePathName(
    EmergencyStopAcceptancePathDTO path) noexcept {
  switch (path) {
    case EmergencyStopAcceptancePathDTO::kFreshReserved:
      return "fresh_reserved";
    case EmergencyStopAcceptancePathDTO::kJoinedResident:
      return "joined_resident";
    case EmergencyStopAcceptancePathDTO::kCoalescedFull:
      return "coalesced_full";
  }
  return {};
}

[[nodiscard]] std::string_view EmergencyStopProjectionStateName(
    EmergencyStopProjectionStateDTO state) noexcept {
  switch (state) {
    case EmergencyStopProjectionStateDTO::kPending:
      return "pending";
    case EmergencyStopProjectionStateDTO::kRecorded:
      return "recorded";
    case EmergencyStopProjectionStateDTO::kFailed:
      return "failed";
  }
  return {};
}

[[nodiscard]] CanonicalValue EmergencyStopAcceptanceRecordValue(
    const EmergencyStopAcceptanceRecordDTO& acceptance) {
  return CanonicalValue(CanonicalValue::Object{
      {"acceptance_path",
       CanonicalValue(std::string(EmergencyStopAcceptancePathName(acceptance.acceptance_path)))},
      {"accepted_at_ns", Timestamp(acceptance.accepted_at_ns, "Stop acceptance timestamp")},
      {"captured_scope_digest", CanonicalValue(acceptance.captured_scope_digest.value())},
      {"entry_state",
       CanonicalValue(std::string(EmergencyStopEntryStateName(acceptance.entry_state)))},
      {"malformed_ingress_reason", OptionalText(acceptance.malformed_ingress_reason)},
      {"normalized_request_digest", OptionalIdentity(acceptance.normalized_request_digest)},
      {"owner_epoch", CanonicalValue(acceptance.owner_epoch.value())},
      {"projection_state",
       CanonicalValue(std::string(EmergencyStopProjectionStateName(acceptance.projection_state)))},
      {"replay_status", CanonicalValue(std::string(to_string(acceptance.replay_status)))},
      {"request_attribution", RequestAttributionValue(acceptance.attribution)},
      {"request_id", CanonicalValue(acceptance.request_id.value())},
      {"resolution_disposition",
       CanonicalValue(std::string(to_string(acceptance.resolution_disposition)))},
      {"stop_generation", Number(acceptance.stop_generation.value(), "Stop generation")},
      {"stop_id", CanonicalValue(acceptance.stop_id.value())},
  });
}

[[nodiscard]] CanonicalValue EmergencyStopAffectedStateValue(
    const EmergencyStopAffectedStateSnapshotDTO& affected_state) {
  CanonicalValue::Array operations;
  operations.reserve(affected_state.operations.size());
  for (const auto& operation : affected_state.operations) {
    operations.emplace_back(OperationStateValue(operation));
  }
  CanonicalValue::Object result{
      {"operations", CanonicalValue(std::move(operations))},
      {"resources", StringArray(affected_state.resources)},
  };
  if (affected_state.motion_takeover_candidate_snapshot)
    result.emplace_back("motion_takeover_candidate_snapshot",
                        OperationStateValue(*affected_state.motion_takeover_candidate_snapshot));
  return CanonicalValue(std::move(result));
}

[[nodiscard]] CanonicalValue EmergencyStopAcceptanceProjectionValue(
    const EmergencyStopAcceptanceRecordDTO& acceptance,
    const EmergencyStopProjectedIntentDTO& intent, const StopAcceptedDTO& accepted,
    const EmergencyStopAffectedStateSnapshotDTO& affected_state) {
  return CanonicalValue(CanonicalValue::Object{
      {"acceptance", EmergencyStopAcceptanceRecordValue(acceptance)},
      {"accepted", StopAcceptedValue(accepted)},
      {"affected_state", EmergencyStopAffectedStateValue(affected_state)},
      {"api_version", CanonicalValue("manyforge.stop_acceptance_projection.v8")},
      {"intent", ProjectedStopIntentValue(intent)},
      {"record_kind", CanonicalValue("stop_acceptance_projected")},
  });
}

[[nodiscard]] CanonicalValue RuntimeScaleRequestValue(const RuntimeScaleRequestDTO& request) {
  return CanonicalValue(CanonicalValue::Object{
      {"expected_operation_revision",
       Number(request.expected_operation_revision.value(), "scale expected operation revision")},
      {"expected_runtime_scale_generation",
       Number(request.expected_runtime_scale_generation.value(),
              "expected runtime scale generation")},
      {"operation_id", CanonicalValue(request.operation_id.value())},
      {"request_attribution", RequestAttributionValue(request.attribution)},
      {"request_context", RequestContextValue(request.request_context)},
      {"scale", CanonicalValue(request.scale)},
  });
}

[[nodiscard]] CanonicalValue RuntimeScaleAcceptedValue(const RuntimeScaleAcceptedDTO& accepted) {
  return CanonicalValue(CanonicalValue::Object{
      {"accepted_at_ns", Timestamp(accepted.accepted_at_ns, "scale acceptance timestamp")},
      {"operation_id", CanonicalValue(accepted.operation_id.value())},
      {"operation_revision", Number(accepted.operation_revision.value(), "operation revision")},
      {"replay_status", CanonicalValue(std::string(to_string(accepted.replay_status)))},
      {"request_id", CanonicalValue(accepted.request_id.value())},
      {"requested_scale", CanonicalValue(accepted.requested_scale)},
      {"runtime_scale_generation",
       Number(accepted.runtime_scale_generation.value(), "runtime scale generation")},
  });
}

[[nodiscard]] CanonicalValue RuntimeScaleStatusValue(const RuntimeScaleStatusDTO* status) {
  if (status == nullptr) {
    return CanonicalValue(nullptr);
  }
  CanonicalValue::Array participants;
  participants.reserve(status->participants.size());
  for (const auto& participant : status->participants) {
    participants.emplace_back(CanonicalValue::Object{
        {"accepted_generation",
         Number(participant.accepted_generation.value(), "accepted runtime scale generation")},
        {"acknowledgement_sequence",
         Number(participant.acknowledgement_sequence, "runtime scale acknowledgement sequence")},
        {"acknowledgement_source_epoch",
         CanonicalValue(participant.acknowledgement_source_epoch.value())},
        {"acknowledgement_timestamp_ns", Timestamp(participant.acknowledgement_timestamp_ns,
                                                   "runtime scale acknowledgement timestamp")},
        {"active_control_configuration_generation",
         Number(participant.active_control_configuration_generation.value(),
                "active control configuration generation")},
        {"active_control_configuration_id",
         CanonicalValue(participant.active_control_configuration_id.value())},
        {"applied_scale", CanonicalValue(participant.applied_scale)},
        {"capability_id", CanonicalValue(participant.capability_id.value())},
        {"command_target_id", CanonicalValue(participant.command_target_id.value())},
        {"commanded_scale", CanonicalValue(participant.commanded_scale)},
        {"control_binding_id", CanonicalValue(participant.control_binding_id.value())},
        {"control_binding_revision", CanonicalValue(participant.control_binding_revision.value())},
        {"control_domain_id", CanonicalValue(participant.control_domain_id.value())},
        {"control_domain_revision", CanonicalValue(participant.control_domain_revision.value())},
        {"control_route_id", CanonicalValue(participant.control_route_id.value())},
        {"controllable_component_id",
         CanonicalValue(participant.controllable_component_id.value())},
        {"controller_goal_generation",
         Number(participant.controller_goal_generation.value(), "controller goal generation")},
        {"controller_goal_id", CanonicalValue(participant.controller_goal_id.value())},
        {"controller_topology_id", CanonicalValue(participant.controller_topology_id.value())},
        {"coordinated_barrier_id", OptionalIdentity(participant.coordinated_barrier_id)},
        {"error", ErrorValue(participant.error)},
        {"participant_id", CanonicalValue(participant.participant_id.value())},
        {"resources", StringArray(participant.resources)},
        {"scaling_domain_id", CanonicalValue(participant.scaling_domain_id.value())},
        {"state", CanonicalValue(to_string(participant.state))},
        {"supported", CanonicalValue(participant.supported)},
        {"topology_revision", CanonicalValue(participant.topology_revision.value())},
        {"virtual_trajectory_time_s", CanonicalValue(participant.virtual_trajectory_time_s)},
    });
  }
  return CanonicalValue(CanonicalValue::Object{
      {"effective_scale", CanonicalValue(status->effective_scale)},
      {"operation_id", CanonicalValue(status->operation_id.value())},
      {"operation_revision", Number(status->operation_revision.value(), "operation revision")},
      {"participants", CanonicalValue(std::move(participants))},
      {"requested_scale", CanonicalValue(status->requested_scale)},
      {"runtime_scale_generation",
       Number(status->runtime_scale_generation.value(), "runtime scale generation")},
      {"state", CanonicalValue(to_string(status->state))},
      {"updated_at_ns", Timestamp(status->updated_at_ns, "runtime scale update timestamp")},
  });
}

[[nodiscard]] CanonicalValue RecordValue(std::string_view record_kind,
                                         const RequestOwnerEpoch& owner_epoch,
                                         const CanonicalValue& intent, const CanonicalValue& result,
                                         const OperationStateDTO& state,
                                         const RuntimeScaleStatusDTO* scale_status,
                                         bool effect_pending) {
  return CanonicalValue(CanonicalValue::Object{
      {"api_version", CanonicalValue("manyforge.operation_journal_record.v8")},
      {"effect_pending", CanonicalValue(effect_pending)},
      {"intent", intent},
      {"operation_state", OperationStateValue(state)},
      {"owner_epoch", CanonicalValue(owner_epoch.value())},
      {"record_kind", CanonicalValue(std::string(record_kind))},
      {"result", result},
      {"runtime_scale_status", RuntimeScaleStatusValue(scale_status)},
  });
}

[[nodiscard]] bool ValidState(const OperationStateDTO& state) noexcept {
  if (state.operation_id.empty() || !state.operation_revision.nonzero() ||
      state.program_binding_id.empty() || state.program_run_id.empty() ||
      state.participants.empty() || state.participants.size() > kMaxOperationParticipants ||
      state.updated_at_ns < 0 ||
      state.journal_capture_order > static_cast<std::uint64_t>(INT64_MAX) ||
      (state.active_stop_id.has_value() != state.active_stop_generation.has_value())) {
    return false;
  }
  if (state.preemption) {
    const auto& fact = *state.preemption;
    if (to_string(fact.kind).empty() || !has_program_execution_scope(state) ||
        state.journal_capture_order == 0U || fact.by.operation_id.empty() ||
        fact.by.operation_id == state.operation_id || fact.by.operation_id.value().size() > 2048U ||
        fact.by.operation_id.value().find('\0') != std::string::npos ||
        !valid_program_node_execution_scope({fact.by.node_instance_id, fact.by.node_attempt_id}))
      return false;
  }
  if (state.motion_takeover &&
      (!state.motion_takeover->locally_valid() ||
       state.motion_takeover->incumbent_operation_id == state.operation_id))
    return false;
  for (const auto& participant : state.participants) {
    if (participant.terminal_fact &&
        participant.terminal_fact->kind == ParticipantExecutionFactKind::kPreemptedByReplacement &&
        (!state.preemption || state.preemption->kind != OperationPreemptionKind::kPreempted ||
         participant.state != OperationStateKind::kStopped ||
         participant.terminal_fact->effect != ParticipantEffectKnowledge::kEffectSettled ||
         participant.terminal_fact->error))
      return false;
    if (participant.terminal_fact &&
        (!IsTerminal(participant.state) ||
         std::string_view(to_string(participant.terminal_fact->kind)) == "unknown" ||
         std::string_view(to_string(participant.terminal_fact->effect)) == "unknown"))
      return false;
    if (participant.execution_scope.has_value() &&
        !valid_program_node_execution_scope(*participant.execution_scope)) {
      return false;
    }
    if (participant.participant_id.empty() || participant.command_id.empty() ||
        !participant.command_generation.nonzero() ||
        participant.controllable_component_id.empty() || participant.command_target_id.empty() ||
        participant.capability_id.empty() || !participant.binding_identity.locally_valid() ||
        !std::isfinite(participant.progress) || participant.progress < 0.0 ||
        participant.progress > 1.0 || participant.resources.empty() ||
        participant.resources.size() > kMaxKernelResourceSlots ||
        std::any_of(participant.resources.begin(), participant.resources.end(),
                    [](const auto& resource) { return resource.empty(); })) {
      return false;
    }
    if (!ValidateIoRuleOutcomeParticipant(state.operation_id, state.operation_revision, participant).ok())
      return false;
    const std::set<ResourceId> unique(participant.resources.begin(), participant.resources.end());
    if (unique.size() != participant.resources.size()) {
      return false;
    }
  }
  return true;
}

void RequireState(const OperationStateDTO& state) {
  Require(ValidState(state), "operation_journal_record_invalid",
          "operation state is invalid at the durable boundary");
}

[[nodiscard]] bool BoundedNfcText(std::string_view value, std::size_t maximum) noexcept {
  if (value.empty() || value.size() > maximum || value.find('\0') != std::string_view::npos) {
    return false;
  }
  try {
    return content_identity::is_utf8_nfc(value);
  } catch (...) {
    return false;
  }
}

[[nodiscard]] bool BoundedIdentity(std::string_view value, std::size_t maximum) noexcept {
  return !value.empty() && value.size() <= maximum && value.find('\0') == std::string_view::npos;
}

[[nodiscard]] bool KnownEmergencyStopRecordEnums(
    const EmergencyStopAcceptanceRecordDTO& acceptance) noexcept {
  return !EmergencyStopEntryStateName(acceptance.entry_state).empty() &&
         !EmergencyStopAcceptancePathName(acceptance.acceptance_path).empty() &&
         !EmergencyStopProjectionStateName(acceptance.projection_state).empty() &&
         !to_string(acceptance.resolution_disposition).empty() &&
         (acceptance.replay_status == RequestReplayStatusDTO::kAcceptedNew ||
          acceptance.replay_status == RequestReplayStatusDTO::kReplayedExact);
}

void RequireEmergencyStopProjection(const RequestOwnerEpoch& owner_epoch,
                                    const EmergencyStopAcceptanceRecordDTO& acceptance,
                                    const EmergencyStopProjectedIntentDTO& intent,
                                    const StopAcceptedDTO& accepted,
                                    const EmergencyStopAffectedStateSnapshotDTO& affected_state) {
  Require(acceptance.owner_epoch == owner_epoch &&
              BoundedIdentity(owner_epoch.value(), kMaximumStopIdentityBytes) &&
              BoundedIdentity(acceptance.request_id.value(), kMaximumEmergencyStopRequestIdBytes) &&
              BoundedIdentity(acceptance.stop_id.value(), kMaximumEmergencyStopIdentityBytes) &&
              acceptance.stop_generation.nonzero() && acceptance.accepted_at_ns >= 0 &&
              content_identity::is_sha256_digest(acceptance.captured_scope_digest.value()) &&
              acceptance.projection_state == EmergencyStopProjectionStateDTO::kPending &&
              KnownEmergencyStopRecordEnums(acceptance) && acceptance.attribution.locally_valid() &&
              (!acceptance.attribution.principal_id.has_value() ||
               BoundedNfcText(*acceptance.attribution.principal_id,
                              kMaximumEmergencyStopPrincipalBytes)),
          "operation_journal_record_invalid",
          "emergency Stop acceptance record is invalid or is not pending projection");

  const bool normalized = acceptance.normalized_request_digest.has_value();
  const bool malformed = acceptance.malformed_ingress_reason.has_value();
  Require(normalized != malformed &&
              (!normalized ||
               content_identity::is_sha256_digest(acceptance.normalized_request_digest->value())) &&
              (!malformed || BoundedNfcText(*acceptance.malformed_ingress_reason,
                                            kMaximumEmergencyStopMalformedReasonBytes)),
          "operation_journal_record_invalid",
          "emergency Stop request identity has an invalid normalized/malformed shape");

  Require(!stop_accepted_field_error(accepted).has_value() &&
              accepted.request_id == acceptance.request_id &&
              accepted.replay_status == acceptance.replay_status &&
              accepted.stop_id == acceptance.stop_id &&
              accepted.stop_generation == acceptance.stop_generation &&
              accepted.accepted_at_ns == acceptance.accepted_at_ns &&
              accepted.resolution_disposition == acceptance.resolution_disposition &&
              accepted.captured_scope_digest == acceptance.captured_scope_digest,
          "operation_journal_record_invalid",
          "complete Stop acceptance does not match its linearized reserve record");

  const auto require_context_join = [&](const auto& selected) {
    Require(selected.request_context.request_owner_epoch == owner_epoch &&
                selected.request_context.request_id == acceptance.request_id &&
                selected.attribution == acceptance.attribution,
            "operation_journal_record_invalid",
            "projected Stop intent does not match its linearized acceptance record");
  };
  std::visit(
      [&](const auto& selected) {
        using Intent = std::decay_t<decltype(selected)>;
        if constexpr (std::is_same_v<Intent, std::monostate>) {
          Require(malformed && !normalized &&
                      acceptance.resolution_disposition ==
                          StopResolutionDispositionDTO::kPromotedToFull &&
                      acceptance.acceptance_path == EmergencyStopAcceptancePathDTO::kCoalescedFull,
                  "operation_journal_record_invalid",
                  "malformed Stop ingress was not conservatively accepted as Full Stop");
        } else if constexpr (std::is_same_v<Intent, StopIntentDTO>) {
          Require(normalized && !malformed && !stop_intent_field_error(selected).has_value(),
                  "operation_journal_record_invalid",
                  "normalized ordinary Stop projection contains an invalid intent");
          require_context_join(selected);
          const auto normalized_digest = ComputeNormalizedStopIntentDigest(selected);
          const auto selector_digest = ComputeRequestedStopSelectorDigest(selected);
          Require(normalized_digest.ok() && selector_digest.ok() &&
                      *acceptance.normalized_request_digest == normalized_digest.value() &&
                      accepted.requested_selector_digest == selector_digest.value(),
                  "operation_journal_record_invalid",
                  "ordinary Stop projection digests do not match the exact intent");
        } else {
          Require(normalized && !malformed && !full_stop_intent_field_error(selected).has_value(),
                  "operation_journal_record_invalid",
                  "normalized Full Stop projection contains an invalid intent");
          require_context_join(selected);
          const auto normalized_digest = ComputeNormalizedStopIntentDigest(selected);
          const auto selector_digest = ComputeRequestedStopSelectorDigest(selected);
          Require(normalized_digest.ok() && selector_digest.ok() &&
                      *acceptance.normalized_request_digest == normalized_digest.value() &&
                      accepted.requested_selector_digest == selector_digest.value(),
                  "operation_journal_record_invalid",
                  "Full Stop projection digests do not match the exact intent");
        }
      },
      intent);

  Require(
      affected_state.operations.size() <= kMaximumOperationJournalHistory &&
          affected_state.resources.size() <= kMaxKernelResourceSlots &&
          std::is_sorted(affected_state.operations.begin(), affected_state.operations.end(),
                         [](const auto& left, const auto& right) {
                           return left.operation_id < right.operation_id;
                         }) &&
          std::adjacent_find(affected_state.operations.begin(), affected_state.operations.end(),
                             [](const auto& left, const auto& right) {
                               return left.operation_id == right.operation_id;
                             }) == affected_state.operations.end() &&
          std::is_sorted(affected_state.resources.begin(), affected_state.resources.end()) &&
          std::adjacent_find(affected_state.resources.begin(), affected_state.resources.end()) ==
              affected_state.resources.end() &&
          std::none_of(affected_state.resources.begin(), affected_state.resources.end(),
                       [](const auto& resource) { return resource.empty(); }),
      "operation_journal_record_invalid",
      "emergency Stop affected-state snapshot is not canonical or bounded");

  Require(affected_state.operations.size() == accepted.affected_operation_revisions.size(),
          "operation_journal_record_invalid",
          "affected-state snapshot does not cover every accepted operation exactly once");
  if (affected_state.motion_takeover_candidate_snapshot) {
    const auto& candidate = *affected_state.motion_takeover_candidate_snapshot;
    RequireState(candidate);
    Require(candidate.motion_takeover &&
                std::none_of(affected_state.operations.begin(), affected_state.operations.end(),
                             [&](const auto& operation) {
                               return operation.operation_id == candidate.operation_id;
                             }) &&
                std::any_of(affected_state.operations.begin(), affected_state.operations.end(),
                            [&](const auto& operation) {
                              return operation.operation_id ==
                                     candidate.motion_takeover->incumbent_operation_id;
                            }),
            "operation_journal_record_invalid",
            "forensic takeover candidate requires a distinct affected incumbent");
  }

  for (std::size_t index = 0U; index < affected_state.operations.size(); ++index) {
    const auto& state = affected_state.operations[index];
    const auto& affected = accepted.affected_operation_revisions[index];
    RequireState(state);
    Require(state.operation_id == affected.operation_id &&
                state.operation_revision == affected.operation_revision_after_acceptance,
            "operation_journal_record_invalid",
            "affected operation state does not match its accepted revision");

    // The originating Stop is this acceptance or one of its joined
    // predecessors (Q51: no per-participant budget rows remain).
    const bool current_origin = state.active_stop_id == accepted.stop_id &&
                                state.active_stop_generation == accepted.stop_generation;
    const bool predecessor_origin = std::any_of(
        accepted.joined_predecessor_stops.begin(), accepted.joined_predecessor_stops.end(),
        [&](const auto& predecessor) {
          return state.active_stop_id == predecessor.predecessor_stop_id &&
                 state.active_stop_generation == predecessor.predecessor_stop_generation;
        });
    Require(current_origin || predecessor_origin, "operation_journal_record_invalid",
            "affected operation does not retain its exact originating Stop");
    for (const auto& participant : state.participants) {
      Require(std::is_sorted(participant.resources.begin(), participant.resources.end()) &&
                  std::all_of(participant.resources.begin(), participant.resources.end(),
                              [&](const auto& resource) {
                                return std::binary_search(affected_state.resources.begin(),
                                                          affected_state.resources.end(), resource);
                              }),
              "operation_journal_record_invalid",
              "affected operation resources are not canonical members of the captured scope");
    }
  }
}

[[nodiscard]] std::filesystem::path ValidatePath(std::filesystem::path path) {
  Require(!path.empty() && path.filename() != "." && path.filename() != "..",
          "operation_journal_path_invalid", "operation journal database path is invalid");
  std::error_code error;
  path = std::filesystem::absolute(path, error).lexically_normal();
  Require(!error, "operation_journal_path_invalid", "operation journal path cannot be resolved");
  const auto parent = std::filesystem::symlink_status(path.parent_path(), error);
  Require(!error && std::filesystem::is_directory(parent) && !std::filesystem::is_symlink(parent),
          "operation_journal_path_invalid",
          "operation journal parent must be an existing non-symlink directory");
  for (const auto& candidate : {path, std::filesystem::path(path.string() + "-wal"),
                                std::filesystem::path(path.string() + "-shm")}) {
    const auto status = std::filesystem::symlink_status(candidate, error);
    if (error == std::errc::no_such_file_or_directory) {
      error.clear();
      continue;
    }
    Require(!error && !std::filesystem::is_symlink(status), "operation_journal_path_invalid",
            "operation journal database or sidecar must not be a symlink");
    if (std::filesystem::exists(status)) {
      Require(std::filesystem::is_regular_file(status), "operation_journal_path_invalid",
              "operation journal database or sidecar must be a regular file");
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

// Lean Stop record (journal v18). `stop_projections` is an index: each Stop
// payload is stored once, in journal_records (payload_json + payload_digest).
// `accepted_payload` is the positional acceptance projection v12 (accepted
// only), with its own SHA-256 in `accepted_digest` (verified on every read,
// as v16 verified acceptance_digest). Every other reader joins the journal row by sequence; with the FKs and
// foreign_keys=ON a non-NULL *_sequence always names an existing row, so a
// NULL joined payload_json means "not yet recorded", as the v16 NULL payload
// columns did. All statements below are the only stop_projections SQL.
constexpr std::string_view kStopProjectionInsertSql =
    "INSERT INTO stop_projections(stop_id,stop_generation,acceptance_sequence,accepted_payload,"
    "accepted_digest,terminal_sequence) "
    "VALUES(?,?,?,?,?,NULL)";
/// Acceptance replay: stop_generation, acceptance_sequence, accepted_payload, accepted_digest.
constexpr std::string_view kStopProjectionAcceptanceSelectSql =
    "SELECT stop_generation,acceptance_sequence,accepted_payload,accepted_digest "
    "FROM stop_projections WHERE stop_id=? AND stop_generation=?";
/// Joined-receipt scope check: accepted_payload, accepted_digest of the creating request.
constexpr std::string_view kStopProjectionAcceptedPayloadSelectSql =
    "SELECT accepted_payload,accepted_digest FROM stop_projections "
    "WHERE stop_id=? AND stop_generation=?";
/// Terminal writer replay and predecessor check: terminal_sequence, terminal payload_json.
constexpr std::string_view kStopProjectionTerminalSelectSql =
    "SELECT p.terminal_sequence,t.payload_json FROM stop_projections p "
    "LEFT JOIN journal_records t ON t.sequence=p.terminal_sequence "
    "WHERE p.stop_id=? AND p.stop_generation=?";
constexpr std::string_view kStopProjectionTerminalUpdateSql =
    "UPDATE stop_projections SET terminal_sequence=? WHERE stop_id=? AND stop_generation=? "
    "AND terminal_sequence IS NULL";
/// Recovery (BeginOwnerEpoch): accepted_payload and accepted_digest, then
/// payload_json and payload_digest of the terminal row. Each present payload
/// is checked against its digest before decoding.
constexpr std::string_view kStopProjectionRecoverySelectSql =
    "SELECT p.accepted_payload,p.accepted_digest,t.payload_json,t.payload_digest "
    "FROM stop_projections p "
    "LEFT JOIN journal_records t ON t.sequence=p.terminal_sequence "
    "ORDER BY p.stop_id,p.stop_generation";
/// CloseOwnerEpoch blockers: terminal payload_json.
constexpr std::string_view kStopProjectionCloseBlockersSelectSql =
    "SELECT t.payload_json FROM stop_projections p "
    "LEFT JOIN journal_records t ON t.sequence=p.terminal_sequence";

/// The `accepted_payload` at `column`, verified against `accepted_digest` at
/// `column + 1`. Every read of the lean acceptance goes through this check.
[[nodiscard]] std::string VerifiedAcceptedPayload(const Statement& row, const int column) {
  auto payload = row.Text(column);
  Require(content_identity::sha256_digest(payload) == row.Text(column + 1),
          "operation_journal_corrupt",
          "Stop acceptance recovery projection digest does not match its bytes");
  return payload;
}

[[nodiscard]] const std::set<std::string>& ExpectedSchemaObjects() {
  static const std::set<std::string> names{
      "index:journal_operation_sequence",
      "index:journal_request_identity",
      "index:journal_stop_acceptance_identity",
      "index:journal_stop_terminal_identity",
      "index:one_active_owner_epoch",
      "index:operation_snapshot_epoch",
      "table:cell_admission_claim_configurations",
      "table:cell_admission_claim_resources",
      "table:cell_admission_claims",
      "table:cell_admission_current_owners",
      "table:cell_admission_reconcile_settlements",
      "table:cell_admission_reconciliations",
      "table:cell_admission_request_membership",
      "table:journal_metadata",
      "table:journal_records",
      "table:operation_error_details",
      "table:operation_participants",
      "table:operation_resources",
      "table:operation_snapshots",
      "table:owner_epochs",
      "table:request_tombstones",
      "table:stop_projections",
  };
  return names;
}

template <typename T>
[[nodiscard]] Result<T> AsResult(auto&& function) {
  try {
    return Result<T>::Ok(function());
  } catch (const ConfigurationCommitUnknown& error) {
    return Result<T>::Err(error.error());
  } catch (const JournalFailure& error) {
    return Result<T>::Err(JournalError(error));
  } catch (const std::exception& error) {
    return Result<T>::Err(ErrorDTO{"operation_journal_internal_failure",
                                   error.what(),
                                   {{"reason", "operation_journal_exception"}}});
  }
}

[[nodiscard]] EmergencyStopAcceptanceProjectionResultDTO AsStopProjectionResult(
    auto&& function) noexcept {
  try {
    return EmergencyStopAcceptanceProjectionResultDTO{EmergencyStopProjectionStateDTO::kRecorded,
                                                      function(), std::nullopt};
  } catch (const JournalFailure& error) {
    return EmergencyStopAcceptanceProjectionResultDTO{EmergencyStopProjectionStateDTO::kFailed,
                                                      std::nullopt, JournalError(error)};
  } catch (const std::exception& error) {
    return EmergencyStopAcceptanceProjectionResultDTO{
        EmergencyStopProjectionStateDTO::kFailed, std::nullopt,
        ErrorDTO{"operation_journal_internal_failure",
                 error.what(),
                 {{"reason", "operation_journal_exception"}}}};
  } catch (...) {
    return EmergencyStopAcceptanceProjectionResultDTO{
        EmergencyStopProjectionStateDTO::kFailed, std::nullopt,
        ErrorDTO{"operation_journal_internal_failure",
                 "unknown operation journal exception",
                 {{"reason", "operation_journal_exception"}}}};
  }
}

[[nodiscard]] StopForensicProjectionResultDTO AsStopForensicProjectionResult(
    auto&& function) noexcept {
  try {
    return StopForensicProjectionResultDTO{StopForensicProjectionStateDTO::kRecorded, function(),
                                           std::nullopt};
  } catch (const JournalFailure& error) {
    return StopForensicProjectionResultDTO{StopForensicProjectionStateDTO::kFailed, std::nullopt,
                                           JournalError(error)};
  } catch (const std::exception& error) {
    return StopForensicProjectionResultDTO{StopForensicProjectionStateDTO::kFailed, std::nullopt,
                                           ErrorDTO{"operation_journal_internal_failure",
                                                    error.what(),
                                                    {{"reason", "operation_journal_exception"}}}};
  } catch (...) {
    return StopForensicProjectionResultDTO{StopForensicProjectionStateDTO::kFailed, std::nullopt,
                                           ErrorDTO{"operation_journal_internal_failure",
                                                    "unknown operation journal exception",
                                                    {{"reason", "operation_journal_exception"}}}};
  }
}

[[nodiscard]] std::uint64_t PositiveIdentifier(std::int64_t value, std::string_view label) {
  Require(value > 0 && static_cast<std::uint64_t>(value) <= kMaximumSafeInteger,
          "operation_journal_corrupt", std::string(label) + " is outside the valid identity range");
  return static_cast<std::uint64_t>(value);
}

[[nodiscard]] std::uint64_t NonnegativeIdentifier(std::int64_t value, std::string_view label) {
  Require(value >= 0 && static_cast<std::uint64_t>(value) <= kMaximumSafeInteger,
          "operation_journal_corrupt",
          std::string(label) + " is outside the valid generation range");
  return static_cast<std::uint64_t>(value);
}

[[nodiscard]] std::optional<OperationPreemptionDTO> CapturedPreemption(
    sqlite3* database, const std::string& captured_state) {
  Statement field(database,
                  "SELECT json_type(?,'$.preemption'),json_extract(?,'$.preemption')");
  field.Bind(1, captured_state);
  field.Bind(2, captured_state);
  Require(field.Step(), "operation_journal_corrupt", "preemption capture is missing");
  if (field.IsNull(0)) return std::nullopt;
  Require(field.Text(0) == "object" && !field.IsNull(1), "operation_journal_corrupt",
          "preemption capture is invalid");
  return PreemptionFromValue(content_identity::parse_canonical_json(field.Text(1)));
}

[[nodiscard]] std::optional<MotionTakeoverDecisionDTO> CapturedMotionTakeover(
    sqlite3* database, const std::string& captured_state) {
  Statement field(database,
                  "SELECT json_type(?,'$.motion_takeover'),json_extract(?,'$.motion_takeover')");
  field.Bind(1, captured_state);
  field.Bind(2, captured_state);
  Require(field.Step(), "operation_journal_corrupt", "motion takeover capture is missing");
  if (field.IsNull(0)) return std::nullopt;
  Require(field.Text(0) == "object" && !field.IsNull(1), "operation_journal_corrupt",
          "motion takeover capture is invalid");
  return MotionTakeoverFromValue(content_identity::parse_canonical_json(field.Text(1)));
}

std::string ValidateSnapshotCapture(sqlite3* database, const OperationId& operation_id,
                                    std::uint64_t order,
                                    const OperationStateDTO* successor = nullptr) {
  Statement record(database,
                   "SELECT payload_json,payload_digest FROM journal_records WHERE sequence="
                   "(SELECT snapshot_sequence FROM operation_snapshots WHERE operation_id=?)");
  record.Bind(1, operation_id.value());
  Require(record.Step(), "operation_journal_corrupt", "current capture record is missing");
  const auto bytes = record.Text(0);
  Require(bytes.size() <= kMaximumRecordJsonBytes &&
              content_identity::sha256_digest(bytes) == record.Text(1) && !record.Step(),
          "operation_journal_corrupt", "current capture record integrity failed");
  // SQLite already owns these bounded JSON records. Extract just the named
  // operation, including a multi-operation Stop capture, without constructing
  // a second large C++ document or trusting an uncommitted projection column.
  Statement captured(database,
                     "SELECT json_extract(?,'$.operation_state') "
                     "WHERE json_extract(?,'$.operation_state.operation_id')=? UNION ALL "
                     "SELECT value "
                     "FROM json_each(?,'$.affected_state.operations') "
                     "WHERE json_extract(value,'$.operation_id')=?");
  captured.Bind(1, bytes);
  captured.Bind(2, bytes);
  captured.Bind(3, operation_id.value());
  captured.Bind(4, bytes);
  captured.Bind(5, operation_id.value());
  Require(captured.Step() && !captured.IsNull(0), "operation_journal_corrupt",
          "operation has no committed capture");
  const auto state = captured.Text(0);
  Require(!captured.Step(), "operation_journal_corrupt", "operation capture is ambiguous");
  Statement capture_version(database, "SELECT json_extract(?,'$.api_version')");
  capture_version.Bind(1, bytes);
  Require(capture_version.Step() &&
              (capture_version.Text(0) == "manyforge.operation_journal_record.v8" ||
               capture_version.Text(0) == "manyforge.stop_acceptance_projection.v8") &&
              !capture_version.Step(),
          "operation_journal_corrupt", "operation capture has an unsupported command identity format");
  if (const auto captured = CapturedPreemption(database, state)) {
    Require(successor == nullptr || successor->preemption == captured,
            "operation_journal_record_invalid", "operation preemption changed after acceptance");
  }


  Statement shape(
      database,
      "SELECT json_extract(?,'$.journal_capture_order'), "
      "json_array_length(?,'$.participants'), "
      "(SELECT COUNT(*) FROM operation_participants WHERE operation_id=?),"
      "json_extract(?,'$.program_run_id'),json_extract(?,'$.program_binding_id'),"
      "program_run_id,program_binding_id FROM operation_snapshots WHERE operation_id=?");
  shape.Bind(1, state);
  shape.Bind(2, state);
  shape.Bind(3, operation_id.value());
  shape.Bind(4, state);
  shape.Bind(5, state);
  shape.Bind(6, operation_id.value());
  Require(shape.Step() &&
              (order == 0U ? shape.IsNull(0) : shape.Text(0) == std::to_string(order)) &&
              !shape.IsNull(1) && shape.Integer(1) == shape.Integer(2) &&
              shape.Text(3) == shape.Text(5) && shape.Text(4) == shape.Text(6),
          "operation_journal_corrupt", "snapshot differs from its committed capture");
  if (successor != nullptr) {
    Require(shape.Text(3) == successor->program_run_id.value() &&
                shape.Text(4) == successor->program_binding_id.value() &&
                shape.Integer(1) == static_cast<std::int64_t>(successor->participants.size()),
            "operation_journal_record_invalid", "operation snapshot changed its captured Program");
  }
  Statement origins(
      database,
      "SELECT p.participant_id,p.execution_node_instance_id,p.execution_node_attempt_id,"
      "json_extract(j.value,'$.participant_id'),json_type(j.value,'$.execution_scope'),"
      "json_extract(j.value,'$.execution_scope.node_instance_id'),"
      "json_extract(j.value,'$.execution_scope.node_attempt_id') "
      "FROM json_each(?,'$.participants') j LEFT JOIN operation_participants p "
      "ON p.operation_id=? AND p.participant_index=CAST(j.key AS INTEGER) "
      "ORDER BY CAST(j.key AS INTEGER)");
  origins.Bind(1, state);
  origins.Bind(2, operation_id.value());
  std::size_t index = 0U;
  while (origins.Step()) {
    const bool scoped = !origins.IsNull(4);
    Require(!origins.IsNull(0) && origins.Text(0) == origins.Text(3) &&
                (scoped ? (origins.Text(4) == "object" && !origins.IsNull(1) &&
                           !origins.IsNull(2) && !origins.IsNull(5) && !origins.IsNull(6) &&
                           origins.Text(1) == origins.Text(5) && origins.Text(2) == origins.Text(6))
                        : (origins.IsNull(1) && origins.IsNull(2))),
            "operation_journal_corrupt", "snapshot origin differs from its committed capture");
    if (successor != nullptr) {
      Require(index < successor->participants.size(), "operation_journal_record_invalid",
              "operation snapshot changed its participant capture");
      const auto& participant = successor->participants[index];
      const auto& scope = participant.execution_scope;
      Require(origins.Text(0) == participant.participant_id.value() &&
                  scoped == scope.has_value() &&
                  (!scoped || (origins.Text(5) == scope->node_instance_id.value() &&
                               origins.Text(6) == scope->node_attempt_id)),
              "operation_journal_record_invalid", "operation snapshot changed its authored origin");
    }
    ++index;
  }
  return state;
}

[[nodiscard]] OperationStateDTO LoadOperationState(sqlite3* database,
                                                   const OperationId& operation_id) {
  Statement snapshot(database,
                     "SELECT operation_revision,program_binding_id,program_run_id,"
                     "coordination_policy,state,updated_at_ns,active_stop_id,"
                     "active_stop_generation,runtime_scale_generation,journal_capture_order "
                     "FROM operation_snapshots WHERE operation_id=?");
  snapshot.Bind(1, operation_id.value());
  Require(snapshot.Step(), "operation_journal_corrupt", "operation snapshot is missing");
  const auto revision = PositiveIdentifier(snapshot.Integer(0), "operation revision");
  const auto coordination = CoordinationFromName(snapshot.Text(3));
  const auto operation_state = OperationStateFromName(snapshot.Text(4));
  Require(coordination.has_value() && operation_state.has_value(), "operation_journal_corrupt",
          "operation snapshot contains an unknown closed-enum value");
  const auto updated_at_ns = snapshot.Integer(5);
  Require(updated_at_ns >= 0, "operation_journal_corrupt",
          "operation snapshot timestamp is invalid");
  const bool has_stop_id = !snapshot.IsNull(6);
  const bool has_stop_generation = !snapshot.IsNull(7);
  Require(has_stop_id == has_stop_generation, "operation_journal_corrupt",
          "operation snapshot has an incomplete Stop identity");
  std::optional<StopId> stop_id;
  std::optional<StopGeneration> stop_generation;
  if (has_stop_id) {
    stop_id = StopId(snapshot.Text(6));
    stop_generation = StopGeneration(NonnegativeIdentifier(snapshot.Integer(7), "Stop generation"));
  }
  std::optional<RuntimeScaleGeneration> runtime_scale_generation;
  if (!snapshot.IsNull(8)) {
    runtime_scale_generation = RuntimeScaleGeneration(
        NonnegativeIdentifier(snapshot.Integer(8), "runtime scale generation"));
  }
  OperationStateDTO state{
      operation_id,
      OperationRevision(revision),
      ProgramBindingId(snapshot.Text(1)),
      ProgramRunId(snapshot.Text(2)),
      *coordination,
      *operation_state,
      {},
      updated_at_ns,
      std::move(stop_id),
      std::move(stop_generation),
      std::move(runtime_scale_generation),
      static_cast<std::uint64_t>(snapshot.Integer(9)),
  };
  Require(!snapshot.Step(), "operation_journal_corrupt",
          "operation snapshot identity is not unique");

  Statement participants(database,
                         "SELECT participant_index,participant_id,command_id,"
                         "command_generation,controllable_component_id,command_target_id,"
                         "capability_id,control_domain_id,control_domain_revision,"
                         "control_binding_id,control_binding_revision,"
                         "capability_provider_binding_id,capability_provider_binding_revision,"
                         "payload_kind,state,progress,terminal_error_code,terminal_error_message,"
                         "execution_node_instance_id,execution_node_attempt_id "
                         "FROM operation_participants WHERE operation_id=? "
                         "ORDER BY participant_index");
  participants.Bind(1, operation_id.value());
  std::int64_t expected_index = 0;
  while (participants.Step()) {
    Require(participants.Integer(0) == expected_index, "operation_journal_corrupt",
            "operation participant indices are not dense and ordered");
    const auto payload_kind = PayloadKindFromName(participants.Text(13));
    const auto participant_state = OperationStateFromName(participants.Text(14));
    Require(payload_kind.has_value() && participant_state.has_value(), "operation_journal_corrupt",
            "operation participant contains an unknown closed-enum value");
    Require(participants.IsNull(18) == participants.IsNull(19), "operation_journal_corrupt",
            "operation participant scope is incomplete");
    std::optional<ProgramNodeExecutionScopeDTO> execution_scope;
    if (!participants.IsNull(18)) {
      execution_scope = ProgramNodeExecutionScopeDTO{ProgramNodeInstanceId(participants.Text(18)),
                                                     participants.Text(19)};
    }
    const bool has_provider_id = !participants.IsNull(11);
    const bool has_provider_revision = !participants.IsNull(12);
    Require(has_provider_id == has_provider_revision, "operation_journal_corrupt",
            "operation participant has an incomplete provider binding");
    std::optional<CapabilityProviderBindingIdentityDTO> provider;
    if (has_provider_id)
      provider = CapabilityProviderBindingIdentityDTO{
          CapabilityProviderBindingId(participants.Text(11)),
          CapabilityProviderBindingRevision(participants.Text(12))};
    ParticipantBindingIdentityDTO binding_identity{
        ControlDomainId(participants.Text(7)), ControlDomainRevision(participants.Text(8)),
        ControllerParticipantBindingIdentityDTO{ControlBindingId(participants.Text(9)),
                                                ControlBindingRevision(participants.Text(10)),
                                                std::move(provider)}};
    const bool has_error_code = !participants.IsNull(16);
    const bool has_error_message = !participants.IsNull(17);
    Require(has_error_code == has_error_message, "operation_journal_corrupt",
            "operation participant has an incomplete terminal error");

    std::vector<ResourceId> resources;
    Statement resource_rows(database,
                            "SELECT resource_index,resource_id FROM operation_resources "
                            "WHERE operation_id=? AND participant_index=? ORDER BY resource_index");
    resource_rows.Bind(1, operation_id.value());
    resource_rows.Bind(2, expected_index);
    std::int64_t expected_resource = 0;
    while (resource_rows.Step()) {
      Require(resource_rows.Integer(0) == expected_resource, "operation_journal_corrupt",
              "operation resource indices are not dense and ordered");
      resources.emplace_back(resource_rows.Text(1));
      ++expected_resource;
    }

    std::optional<ErrorDTO> terminal_error;
    if (has_error_code) {
      ErrorDTO decoded{participants.Text(16), participants.Text(17), {}};
      Statement details(database,
                        "SELECT detail_key,detail_value FROM operation_error_details "
                        "WHERE operation_id=? AND participant_index=? ORDER BY detail_key");
      details.Bind(1, operation_id.value());
      details.Bind(2, expected_index);
      while (details.Step()) {
        const auto [unused, inserted] = decoded.details.emplace(details.Text(0), details.Text(1));
        (void)unused;
        Require(inserted, "operation_journal_corrupt",
                "operation terminal error has duplicate detail keys");
      }
      terminal_error = std::move(decoded);
    }

    state.participants.push_back(OperationParticipantStateDTO{
        OperationParticipantId(participants.Text(1)),
        ControllerGoalId(participants.Text(2)),
        ControllerGoalGeneration(
            PositiveIdentifier(participants.Integer(3), "command generation")),
        ControllableComponentId(participants.Text(4)),
        CommandTargetId(participants.Text(5)),
        CapabilityId(participants.Text(6)),
        std::move(binding_identity),
        *payload_kind,
        *participant_state,
        participants.Real(15),
        std::move(resources),
        std::move(terminal_error),
        std::move(execution_scope),
    });
    ++expected_index;
  }
  const auto capture = ValidateSnapshotCapture(database, operation_id, state.journal_capture_order);
  state.preemption = CapturedPreemption(database, capture);
  state.motion_takeover = CapturedMotionTakeover(database, capture);
  Statement facts(
      database,
      "SELECT json_type(value,'$.terminal_fact'),json_extract(value,'$.terminal_fact'),"
      "json_extract(value,'$.participant_id'),json_extract(value,'$.command_id'),"
      "json_extract(value,'$.command_generation'),json_extract(value,'$.control_binding_id'"
      "),"
      "json_extract(value,'$.control_binding_revision'),json_extract(value,'$.control_domain_id'),"
      "json_extract(value,'$.control_domain_revision'),json_extract(value,'$.capability_provider_"
      "binding_id'),"
      "json_extract(value,'$.capability_provider_binding_revision'),"
      "json_type(value,'$.io_rule_outcomes'),json_extract(value,'$.io_rule_outcomes') "
      "FROM json_each(?,'$.participants') ORDER BY CAST(key AS INTEGER)");
  facts.Bind(1, capture);
  std::size_t fact_index = 0U;
  while (facts.Step()) {
    Require(fact_index < state.participants.size(), "operation_journal_corrupt",
            "participant fact capture count mismatch");
    auto& participant = state.participants[fact_index];
    const auto& controller = participant.binding_identity.controller;
    const auto* provider = participant.binding_identity.capability_provider();
    Require(facts.Text(2) == participant.participant_id.value() &&
                facts.Text(3) == participant.command_id.value() &&
                facts.Integer(4) ==
                    static_cast<std::int64_t>(participant.command_generation.value()) &&
                !facts.IsNull(5) && !facts.IsNull(6) &&
                facts.Text(5) == controller.control_binding_id.value() &&
                facts.Text(6) == controller.control_binding_revision.value() &&
                facts.Text(7) == participant.binding_identity.control_domain_id.value() &&
                facts.Text(8) == participant.binding_identity.control_domain_revision.value() &&
                facts.IsNull(9) == (provider == nullptr) &&
                facts.IsNull(10) == (provider == nullptr) &&
                (!provider ||
                 (facts.Text(9) == provider->capability_provider_binding_id.value() &&
                  facts.Text(10) == provider->capability_provider_binding_revision.value())),
            "operation_journal_corrupt", "participant capture identity mismatch");
    if (!facts.IsNull(0)) {
      Require(facts.Text(0) == "object" && !facts.IsNull(1), "operation_journal_corrupt",
              "participant fact capture shape mismatch");
      participant.terminal_fact =
          ParticipantFactFromValue(content_identity::parse_canonical_json(facts.Text(1)));
    }
    if (!facts.IsNull(11)) {
      Require(facts.Text(11) == "array" && !facts.IsNull(12), "operation_journal_corrupt",
              "participant rule outcome shape mismatch");
      auto decoded = DecodeIoRuleOutcomes(facts.Text(12));
      Require(decoded.ok(), "operation_journal_corrupt", "participant rule evidence is invalid");
      participant.io_rule_outcomes = std::move(decoded.value());
      Require(ValidateIoRuleOutcomeParticipant(state.operation_id, state.operation_revision,
                  participant).ok(), "operation_journal_corrupt", "participant rule evidence relabels identity");
    }
    ++fact_index;
  }
  Require(fact_index == state.participants.size(), "operation_journal_corrupt",
          "participant fact capture count mismatch");
  RequireState(state);
  return state;
}

void StoreOperationSnapshot(sqlite3* database, const RequestOwnerEpoch& owner_epoch,
                            const OperationStateDTO& state, std::uint64_t snapshot_sequence,
                            std::string_view disposition, std::string_view capture_digest) {
  RequireState(state);
  Require(snapshot_sequence > 0U && snapshot_sequence <= kMaximumSafeInteger,
          "operation_journal_record_invalid", "operation journal sequence is invalid");
  Statement previous(database,
                     "SELECT journal_capture_order,operation_revision FROM operation_snapshots "
                     "WHERE operation_id=?");
  previous.Bind(1, state.operation_id.value());
  if (previous.Step()) {
    const auto order = previous.Integer(0);
    const auto revision = previous.Integer(1);
    Require(order >= 0 && !previous.Step(), "operation_journal_corrupt",
            "operation snapshot capture order is invalid");
    ValidateSnapshotCapture(database, state.operation_id, static_cast<std::uint64_t>(order),
                            &state);
    if (order > 0) {
      if (state.journal_capture_order < static_cast<std::uint64_t>(order)) return;
      if (state.journal_capture_order == static_cast<std::uint64_t>(order)) {
        Require(LoadOperationState(database, state.operation_id) == state,
                "operation_journal_corrupt", "equal capture order names conflicting state");
        Statement record(
            database,
            "SELECT payload_json,payload_digest FROM journal_records WHERE sequence="
            "(SELECT snapshot_sequence FROM operation_snapshots WHERE operation_id=?)");
        record.Bind(1, state.operation_id.value());
        Require(record.Step(), "operation_journal_corrupt", "current capture record is missing");
        const auto bytes = record.Text(0);
        const auto digest = record.Text(1);
        Require(content_identity::sha256_digest(bytes) == digest && !record.Step(),
                "operation_journal_corrupt", "current capture record integrity failed");
        Require(digest == capture_digest, "operation_journal_corrupt",
                "equal capture order names conflicting journal contents");
        return;
      }
      Require(state.operation_revision.value() >= static_cast<std::uint64_t>(revision),
              "operation_journal_corrupt", "newer capture regresses the public operation revision");
    }
  }
  Statement upsert(database,
                   "INSERT INTO operation_snapshots("
                   "operation_id,owner_epoch,operation_revision,program_binding_id,"
                   "program_run_id,coordination_policy,state,updated_at_ns,active_stop_id,"
                   "active_stop_generation,runtime_scale_generation,disposition,"
                   "snapshot_sequence,journal_capture_order) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?) "
                   "ON CONFLICT(operation_id) DO UPDATE SET owner_epoch=excluded.owner_epoch,"
                   "operation_revision=excluded.operation_revision,"
                   "program_binding_id=excluded.program_binding_id,"
                   "program_run_id=excluded.program_run_id,"
                   "coordination_policy=excluded.coordination_policy,state=excluded.state,"
                   "updated_at_ns=excluded.updated_at_ns,active_stop_id=excluded.active_stop_id,"
                   "active_stop_generation=excluded.active_stop_generation,"
                   "runtime_scale_generation=excluded.runtime_scale_generation,"
                   "disposition=excluded.disposition,"
                   "snapshot_sequence=excluded.snapshot_sequence,"
                   "journal_capture_order=excluded.journal_capture_order "
                   "WHERE excluded.operation_revision>=operation_snapshots.operation_revision");
  upsert.Bind(1, state.operation_id.value());
  upsert.Bind(2, owner_epoch.value());
  upsert.Bind(3, static_cast<std::int64_t>(state.operation_revision.value()));
  upsert.Bind(4, state.program_binding_id.value());
  upsert.Bind(5, state.program_run_id.value());
  upsert.Bind(6, CoordinationName(state.coordination_policy));
  upsert.Bind(7, OperationStateName(state.state));
  upsert.Bind(8, state.updated_at_ns);
  if (state.active_stop_id.has_value()) {
    upsert.Bind(9, state.active_stop_id->value());
    upsert.Bind(10, static_cast<std::int64_t>(state.active_stop_generation->value()));
  } else {
    upsert.BindNull(9);
    upsert.BindNull(10);
  }
  if (state.runtime_scale_generation.has_value()) {
    upsert.Bind(11, static_cast<std::int64_t>(state.runtime_scale_generation->value()));
  } else {
    upsert.BindNull(11);
  }
  upsert.Bind(12, disposition);
  upsert.Bind(13, static_cast<std::int64_t>(snapshot_sequence));
  upsert.Bind(14, static_cast<std::int64_t>(state.journal_capture_order));
  Require(!upsert.Step(), "operation_journal_corrupt",
          "operation snapshot upsert returned an unexpected row");
  if (sqlite3_changes(database) == 0) return;

  Statement remove_participants(database,
                                "DELETE FROM operation_participants WHERE operation_id=?");
  remove_participants.Bind(1, state.operation_id.value());
  Require(!remove_participants.Step(), "operation_journal_corrupt",
          "operation participant replacement returned an unexpected row");

  for (std::size_t index = 0; index < state.participants.size(); ++index) {
    const auto& participant = state.participants[index];
    Statement insert_participant(
        database,
        "INSERT INTO operation_participants("
        "operation_id,participant_index,participant_id,command_id,"
        "command_generation,controllable_component_id,command_target_id,"
        "capability_id,control_domain_id,control_domain_revision,control_binding_id,"
        "control_binding_revision,capability_provider_binding_id,"
        "capability_provider_binding_revision,payload_kind,state,progress,"
        "terminal_error_code,terminal_error_message,execution_node_instance_id,execution_node_"
        "attempt_id) "
        "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)");
    insert_participant.Bind(1, state.operation_id.value());
    insert_participant.Bind(2, static_cast<std::int64_t>(index));
    insert_participant.Bind(3, participant.participant_id.value());
    insert_participant.Bind(4, participant.command_id.value());
    insert_participant.Bind(
        5, static_cast<std::int64_t>(participant.command_generation.value()));
    insert_participant.Bind(6, participant.controllable_component_id.value());
    insert_participant.Bind(7, participant.command_target_id.value());
    insert_participant.Bind(8, participant.capability_id.value());
    insert_participant.Bind(9, participant.binding_identity.control_domain_id.value());
    insert_participant.Bind(10, participant.binding_identity.control_domain_revision.value());
    insert_participant.Bind(11, participant.binding_identity.controller.control_binding_id.value());
    insert_participant.Bind(12, participant.binding_identity.controller.control_binding_revision.value());
    if (const auto* provider = participant.binding_identity.capability_provider()) {
      insert_participant.Bind(13, provider->capability_provider_binding_id.value());
      insert_participant.Bind(14, provider->capability_provider_binding_revision.value());
    } else {
      insert_participant.BindNull(13);
      insert_participant.BindNull(14);
    }
    insert_participant.Bind(15, PayloadKindName(participant.payload_kind));
    insert_participant.Bind(16, OperationStateName(participant.state));
    insert_participant.Bind(17, participant.progress);
    if (participant.terminal_error.has_value()) {
      insert_participant.Bind(18, participant.terminal_error->code);
      insert_participant.Bind(19, participant.terminal_error->message);
    } else {
      insert_participant.BindNull(18);
      insert_participant.BindNull(19);
    }
    if (participant.execution_scope.has_value()) {
      insert_participant.Bind(20, participant.execution_scope->node_instance_id.value());
      insert_participant.Bind(21, participant.execution_scope->node_attempt_id);
    } else {
      insert_participant.BindNull(20);
      insert_participant.BindNull(21);
    }
    Require(!insert_participant.Step(), "operation_journal_corrupt",
            "operation participant insert returned an unexpected row");

    for (std::size_t resource_index = 0; resource_index < participant.resources.size();
         ++resource_index) {
      Statement insert_resource(database,
                                "INSERT INTO operation_resources(operation_id,participant_index,"
                                "resource_index,resource_id) VALUES(?,?,?,?)");
      insert_resource.Bind(1, state.operation_id.value());
      insert_resource.Bind(2, static_cast<std::int64_t>(index));
      insert_resource.Bind(3, static_cast<std::int64_t>(resource_index));
      insert_resource.Bind(4, participant.resources[resource_index].value());
      Require(!insert_resource.Step(), "operation_journal_corrupt",
              "operation resource insert returned an unexpected row");
    }
    if (participant.terminal_error.has_value()) {
      for (const auto& [key, value] : participant.terminal_error->details) {
        Statement insert_detail(
            database,
            "INSERT INTO operation_error_details(operation_id,participant_index,"
            "detail_key,detail_value) VALUES(?,?,?,?)");
        insert_detail.Bind(1, state.operation_id.value());
        insert_detail.Bind(2, static_cast<std::int64_t>(index));
        insert_detail.Bind(3, key);
        insert_detail.Bind(4, value);
        Require(!insert_detail.Step(), "operation_journal_corrupt",
                "operation error detail insert returned an unexpected row");
      }
    }
  }
}

/// An asynchronous Stop projection may arrive after a later operation-state
/// projection. Retain the acceptance event, but never let its captured state
/// regress the recovery snapshot.
void StoreOperationSnapshotMonotonic(sqlite3* database, const RequestOwnerEpoch& owner_epoch,
                                     const OperationStateDTO& state,
                                     std::uint64_t snapshot_sequence,
                                     std::string_view capture_digest) {
  if (state.journal_capture_order != 0U) {
    StoreOperationSnapshot(database, owner_epoch, state, snapshot_sequence, "current",
                           capture_digest);
    return;
  }
  Statement present(database, "SELECT COUNT(*) FROM operation_snapshots WHERE operation_id=?");
  present.Bind(1, state.operation_id.value());
  Require(present.Step(), "operation_journal_corrupt",
          "operation snapshot presence query returned no row");
  const auto count = present.Integer(0);
  Require(!present.Step() && (count == 0 || count == 1), "operation_journal_corrupt",
          "operation snapshot presence query is invalid");
  if (count == 0) {
    StoreOperationSnapshot(database, owner_epoch, state, snapshot_sequence, "current",
                           capture_digest);
    return;
  }

  const auto current = LoadOperationState(database, state.operation_id);
  // A delayed unsequenced legacy capture cannot replace an ordered one,
  // including at the same public revision and clock. It is old, not corrupt.
  if (current.journal_capture_order != 0U) return;
  if (current.operation_revision.value() > state.operation_revision.value() ||
      (current.operation_revision == state.operation_revision &&
       current.updated_at_ns > state.updated_at_ns)) {
    return;
  }
  if (current.operation_revision == state.operation_revision &&
      current.updated_at_ns == state.updated_at_ns) {
    Require(current == state, "operation_journal_corrupt",
            "equal operation revision/timestamp names conflicting immutable state");
    return;
  }
  StoreOperationSnapshot(database, owner_epoch, state, snapshot_sequence, "current",
                         capture_digest);
}

[[nodiscard]] CellAdmissionClaimKindDTO CellClaimKind(std::string_view value) {
  if (value == "program_cycle") return CellAdmissionClaimKindDTO::kProgramCycle;
  if (value == "program_reset") return CellAdmissionClaimKindDTO::kProgramReset;
  if (value == "authoring_mutation") return CellAdmissionClaimKindDTO::kAuthoringMutation;
  if (value == "manager_apply") return CellAdmissionClaimKindDTO::kManagerApply;
  if (value == "authored_scene") return CellAdmissionClaimKindDTO::kAuthoredScene;
  if (value == "manual_operation") return CellAdmissionClaimKindDTO::kManualOperation;
  if (value == "intervention_or_recovery")
    return CellAdmissionClaimKindDTO::kInterventionOrRecovery;
  Fail("operation_journal_corrupt", "unknown durable cell-admission claim kind");
}

[[nodiscard]] CellAdmissionOwnerRoleDTO CellOwnerRole(std::string_view value) {
  if (value == "program_runner") return CellAdmissionOwnerRoleDTO::kProgramRunner;
  if (value == "authoring_owner") return CellAdmissionOwnerRoleDTO::kAuthoringOwner;
  if (value == "manager") return CellAdmissionOwnerRoleDTO::kManager;
  if (value == "authored_scene_owner") return CellAdmissionOwnerRoleDTO::kAuthoredSceneOwner;
  if (value == "manual_operation_owner") return CellAdmissionOwnerRoleDTO::kManualOperationOwner;
  if (value == "intervention_or_recovery_owner")
    return CellAdmissionOwnerRoleDTO::kInterventionOrRecoveryOwner;
  Fail("operation_journal_corrupt", "unknown durable cell-admission owner role");
}

[[nodiscard]] CellAdmissionSettlementKindDTO CellSettlementKind(std::string_view value) {
  if (value == "owner_commit") return CellAdmissionSettlementKindDTO::kOwnerCommit;
  if (value == "proven_no_effect") return CellAdmissionSettlementKindDTO::kProvenNoEffect;
  if (value == "terminal_effects_settled")
    return CellAdmissionSettlementKindDTO::kTerminalEffectsSettled;
  if (value == "explicit_reset") return CellAdmissionSettlementKindDTO::kExplicitReset;
  if (value == "reconciled") return CellAdmissionSettlementKindDTO::kReconciled;
  Fail("operation_journal_corrupt", "unknown durable cell-admission settlement kind");
}

[[nodiscard]] OperationJournalRecoveryDTO::RecoveredCellAdmissionClaimDTO LoadCellClaim(
    sqlite3* database, std::uint64_t generation, bool admission_timing = false,
    bool release_timing = false) {
  Statement row(
      database,
      "SELECT claim_kind,owner_role,owner_epoch,owner_transaction_id,request_id,"
      "request_payload_digest,expected_admission_generation,acquired_admission_generation,"
      "accepted_at_ns,draft_program_revision,applied_program_revision,offering_owner_role,"
      "offering_owner_epoch,offer_transaction_id,offer_digest,"
      "parent_program_cycle_claim_generation,release_owner_role,release_owner_epoch,"
      "release_owner_transaction_id,release_request_id,release_request_payload_digest,"
      "release_acquired_admission_generation,release_settlement_kind,release_evidence_digest,"
      "program_node_runner_owner_epoch,program_node_run_id,program_node_instance_id,"
      "program_node_attempt_id,program_node_owner_api "
      "FROM cell_admission_claims WHERE claim_generation=?", admission_timing ? (release_timing ? "release_load_claim_row_step"
                                                      : "acquire_load_claim_row_step")
                                       : nullptr);
  row.Bind(1, static_cast<std::int64_t>(generation));
  Require(row.Step(), "operation_journal_corrupt", "cell-admission claim row is missing");
  CellAdmissionClaimIntentDTO intent{
      CellClaimKind(row.Text(0)),
      CellOwnerRole(row.Text(1)),
      RequestOwnerEpoch(row.Text(2)),
      row.Text(3),
      RequestId(row.Text(4)),
      ContentDigest(row.Text(5)),
      AdmissionGeneration(PositiveIdentifier(row.Integer(6), "admission generation")),
      {},
      std::nullopt,
      std::nullopt};
  if (!row.IsNull(9)) {
    intent.program_cycle_evidence = ProgramCycleAdmissionEvidenceDTO{ProgramRevision(row.Text(9)),
                                                                     ProgramRevision(row.Text(10))};
  }
  if (!row.IsNull(11)) {
    CellAdmissionInterventionEvidenceDTO evidence{CellOwnerRole(row.Text(11)),
                                                  RequestOwnerEpoch(row.Text(12)), row.Text(13),
                                                  ContentDigest(row.Text(14)), std::nullopt};
    if (!row.IsNull(15)) {
      evidence.parent_program_cycle_claim_generation = CellAdmissionClaimGeneration(
          PositiveIdentifier(row.Integer(15), "parent cell-admission claim generation"));
    }
    intent.intervention_evidence = std::move(evidence);
  }
  const bool program_node = !row.IsNull(24);
  for (int index = 25; index <= 28; ++index) {
    Require(row.IsNull(index) != program_node, "operation_journal_corrupt",
            "incomplete Program node claim attribution");
  }
  if (program_node) {
    Require(!row.IsNull(15), "operation_journal_corrupt", "Program node claim has no parent");
    intent.program_node_evidence =
        ProgramNodeAdmissionEvidenceDTO{RequestOwnerEpoch(row.Text(24)),
                                        ProgramRunId(row.Text(25)),
                                        CellAdmissionClaimGeneration(PositiveIdentifier(
                                            row.Integer(15), "parent claim generation")),
                                        row.Text(26),
                                        row.Text(27),
                                        row.Text(28)};
  } else if (!intent.intervention_evidence.has_value()) {
    Require(row.IsNull(15), "operation_journal_corrupt", "orphan parent claim attribution");
  }
  Statement resources(database,
                      "SELECT resource_id FROM cell_admission_claim_resources "
                      "WHERE claim_generation=? ORDER BY resource_index", admission_timing ? (release_timing ? "release_load_claim_resources_step"
                                                      : "acquire_load_claim_resources_step")
                                       : nullptr);
  resources.Bind(1, static_cast<std::int64_t>(generation));
  while (resources.Step()) {
    intent.scope.resources.emplace_back(resources.Text(0));
  }
  Statement configurations(
      database,
      "SELECT control_domain_id,active_configuration_id,active_configuration_generation "
      "FROM cell_admission_claim_configurations WHERE claim_generation=? "
      "ORDER BY configuration_index", admission_timing ? (release_timing ? "release_load_claim_configurations_step"
                                                      : "acquire_load_claim_configurations_step")
                                       : nullptr);
  configurations.Bind(1, static_cast<std::int64_t>(generation));
  while (configurations.Step()) {
    intent.scope.configurations.push_back(CellAdmissionConfigurationScopeDTO{
        ControlDomainId(configurations.Text(0)),
        ActiveControlConfigurationId(configurations.Text(1)),
        ActiveControlConfigurationGeneration(
            PositiveIdentifier(configurations.Integer(2), "active configuration generation"))});
  }
  CellAdmissionClaimDTO claim{
      std::move(intent), CellAdmissionClaimGeneration(generation),
      AdmissionGeneration(PositiveIdentifier(row.Integer(7), "acquired admission generation")),
      row.Integer(8)};
  std::optional<CellAdmissionClaimReleaseDTO> release;
  if (!row.IsNull(16)) {
    release = CellAdmissionClaimReleaseDTO{
        CellOwnerRole(row.Text(16)),
        RequestOwnerEpoch(row.Text(17)),
        row.Text(18),
        RequestId(row.Text(19)),
        ContentDigest(row.Text(20)),
        AdmissionGeneration(PositiveIdentifier(row.Integer(21), "release admission generation")),
        CellAdmissionClaimGeneration(generation),
        CellAdmissionSettlementEvidenceDTO{CellSettlementKind(row.Text(22)),
                                           ContentDigest(row.Text(23))}};
  }
  Require(!row.Step() && ValidateCellAdmissionClaimIntent(claim.intent).ok() &&
              (!release.has_value() || ValidateCellAdmissionClaimRelease(*release).ok()),
          "operation_journal_corrupt", "durable cell-admission claim is invalid");
  return {std::move(claim), std::move(release), std::nullopt};
}

[[nodiscard]] std::optional<CellAdmissionReconcileSettlementDTO> LoadCellReconcileSettlement(
    sqlite3* database, const CellAdmissionClaimDTO& claim) {
  Statement row(database,
                "SELECT settlement.successor_owner_role,settlement.successor_owner_epoch,"
                "settlement.settlement_predicate,settlement.evidence_digest "
                "FROM cell_admission_reconcile_settlements settlement "
                "JOIN cell_admission_claims claim "
                "ON claim.reconciled_sequence=settlement.reconcile_sequence "
                "AND claim.claim_generation=settlement.claim_generation "
                "WHERE settlement.claim_generation=?");
  row.Bind(1, static_cast<std::int64_t>(claim.claim_generation.value()));
  if (!row.Step()) {
    return std::nullopt;
  }
  CellAdmissionReconcileSettlementDTO result{
      claim, CellOwnerRole(row.Text(0)), RequestOwnerEpoch(row.Text(1)),
      CellSettlementKind(row.Text(2)), ContentDigest(row.Text(3))};
  Require(!row.Step() && result.successor_owner_role == claim.intent.owner_role &&
              result.successor_owner_epoch != claim.intent.owner_epoch &&
              result.settlement_predicate == CellAdmissionSettlementKindDTO::kReconciled &&
              content_identity::is_sha256_digest(result.reconciliation_evidence_digest.value()),
          "operation_journal_corrupt", "durable cell-admission reconcile settlement is invalid");
  return result;
}

[[nodiscard]] std::vector<CellAdmissionClaimDTO> LoadHeldCellClaims(sqlite3* database,
                                                                    bool admission_timing = false) {
  std::vector<CellAdmissionClaimDTO> rows;
  Statement held(database,
                 "SELECT claim_generation FROM cell_admission_claims "
                 "WHERE release_owner_role IS NULL AND reconciled_sequence IS NULL "
                 "ORDER BY claim_generation", admission_timing ? "acquire_held_claims_step" : nullptr);
  while (held.Step()) {
    rows.push_back(LoadCellClaim(database, PositiveIdentifier(held.Integer(0),
                                                              "cell-admission claim generation"),
                                 admission_timing)
                       .claim);
  }
  return rows;
}

[[nodiscard]] std::vector<CellAdmissionCurrentOwnerEpochDTO> LoadCellCurrentOwners(
    sqlite3* database) {
  std::vector<CellAdmissionCurrentOwnerEpochDTO> rows;
  Statement owners(database, "SELECT owner_role,owner_epoch FROM cell_admission_current_owners");
  while (owners.Step()) {
    rows.push_back({CellOwnerRole(owners.Text(0)), RequestOwnerEpoch(owners.Text(1))});
  }
  std::sort(rows.begin(), rows.end(),
            [](const auto& a, const auto& b) { return a.owner_role < b.owner_role; });
  return rows;
}

[[nodiscard]] bool ConfigurationApplyClaim(const CellAdmissionClaimDTO& claim) {
  return claim.intent.claim_kind == CellAdmissionClaimKindDTO::kManagerApply &&
         claim.intent.owner_role == CellAdmissionOwnerRoleDTO::kManager &&
         claim.intent.scope.resources.empty() && claim.intent.scope.configurations.empty() &&
         !claim.intent.program_cycle_evidence && !claim.intent.intervention_evidence &&
         !claim.intent.program_node_evidence;
}

}  // namespace

class SqliteOperationJournal::Impl final {
 public:
  Impl(std::filesystem::path database_path, CellId cell_id, std::size_t history_capacity,
       std::function<std::string()> wall_time_text,
       std::function<void(std::string_view)> critical_section_observer)
      : database_path_(ValidatePath(std::move(database_path))),
        cell_id_(std::move(cell_id)),
        history_capacity_(history_capacity),
        wall_time_text_(wall_time_text ? std::move(wall_time_text) : ProductionWallTimeText),
        critical_section_observer_(std::move(critical_section_observer)) {
    Require(!cell_id_.empty() && history_capacity_ > 0U &&
                history_capacity_ <= kMaximumOperationJournalHistory,
            "operation_journal_configuration_invalid",
            "operation journal cell identity or history bound is invalid");
    Require(sqlite3_libversion_number() >= 3037000, "operation_journal_storage_unsupported",
            "operation journal requires SQLite 3.37.0 or newer");
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
      Fail("operation_journal_storage_failure", "open operation journal database: " + detail);
    }
    try {
      Configure();
      InitializeOrValidate();
      OpenReader();
    } catch (...) {
      if (reader_ != nullptr) {
        sqlite3_close(reader_);
        reader_ = nullptr;
      }
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
    const auto lock = LockOrdinaryWriter();
    if (closed_) {
      return;
    }
    {
      // The reader holds no writer authority; retire it before the final
      // truncating checkpoint so no read snapshot pins the WAL.
      std::lock_guard reader_lock(reader_mutex_);
      if (reader_ != nullptr) {
        const auto closed_reader = sqlite3_close(reader_);
        if (closed_reader != SQLITE_OK) {
          Fail("operation_journal_storage_failure",
               "close operation journal reader: " + SqliteDetail(reader_, closed_reader));
        }
        reader_ = nullptr;
      }
    }
    sqlite3_wal_checkpoint_v2(database_, nullptr, SQLITE_CHECKPOINT_TRUNCATE, nullptr, nullptr);
    const auto result = sqlite3_close(database_);
    if (result != SQLITE_OK) {
      Fail("operation_journal_storage_failure",
           "close operation journal database: " + SqliteDetail(database_, result));
    }
    database_ = nullptr;
    closed_ = true;
  }

  OperationJournalRecoveryDTO BeginOwnerEpoch(const RequestOwnerEpoch& owner_epoch,
                                              std::int64_t started_at_ns,
                                              std::size_t request_capacity,
                                              const std::vector<ResourceId>& resource_inventory);

  std::uint64_t RecordOperationAccepted(const RequestOwnerEpoch& owner_epoch,
                                        const OperationIntentDTO& intent,
                                        const OperationAcceptedDTO& accepted,
                                        const OperationStateDTO& state,
                                        std::span<const OperationId> protected_operation_ids);

  std::uint64_t RecordControlAccepted(const RequestOwnerEpoch& owner_epoch,
                                      const OperationControlRequestDTO& request,
                                      const OperationControlAcceptedDTO& accepted,
                                      const OperationStateDTO& state);

  std::uint64_t RecordStopAccepted(const RequestOwnerEpoch& owner_epoch,
                                   const EmergencyStopAcceptanceRecordDTO& acceptance,
                                   const EmergencyStopProjectedIntentDTO& intent,
                                   const StopAcceptedDTO& accepted,
                                   const EmergencyStopAffectedStateSnapshotDTO& affected_state);

  std::uint64_t RecordStopTerminal(const RequestOwnerEpoch& owner_epoch,
                                   const StopStatusDTO& stop_status,
                                   const StopCertificateDTO& certificate);

  std::uint64_t CloseOwnerEpoch(const RequestOwnerEpoch& owner_epoch, std::int64_t closed_at_ns);

  std::uint64_t RecordRuntimeScaleAccepted(const RequestOwnerEpoch& owner_epoch,
                                           const RuntimeScaleRequestDTO& request,
                                           const RuntimeScaleAcceptedDTO& accepted,
                                           const OperationStateDTO& state,
                                           const RuntimeScaleStatusDTO& scale_status);

  std::uint64_t RecordOperationState(
      const RequestOwnerEpoch& owner_epoch, const OperationStateDTO& state,
      const RuntimeScaleStatusDTO* scale_status, const char* transition_reason,
      std::span<const PreparedMotionDiagnosticsDTO> prepared_motion_diagnostics);

  std::uint64_t RecordCellAdmissionClaimAcquired(
      const RequestOwnerEpoch& owner_epoch, const CellAdmissionClaimDTO& claim,
      const CellAdmissionCurrentOwnerEpochDTO* verified_manager = nullptr,
      const std::vector<CellAdmissionClaimDTO>* expected_held_claims = nullptr,
      const std::vector<CellAdmissionCurrentOwnerEpochDTO>* expected_current_owners = nullptr,
      CellAdmissionClaimDTO* persisted_claim = nullptr);
  AdmissionGeneration RecordTerminalApplyClaimsReleasedBySuccess(
      const RequestOwnerEpoch& owner_epoch, const CellAdmissionClaimDTO& fresh_claim,
      const CellAdmissionClaimReleaseDTO& fresh_success_release,
      const std::vector<CellAdmissionClaimDTO>& expected_held_claims,
      const std::vector<CellAdmissionCurrentOwnerEpochDTO>& expected_current_owners,
      AdmissionGeneration expected_admission_generation);
  std::uint64_t RecordCellAdmissionClaimReleased(const RequestOwnerEpoch& owner_epoch,
                                                 const CellAdmissionClaimDTO& claim,
                                                 const CellAdmissionClaimReleaseDTO& release,
                                                 AdmissionGeneration resulting_generation);
  std::uint64_t RecordCellAdmissionClaimsReconciled(
      const RequestOwnerEpoch& owner_epoch, const CellAdmissionReconcileDTO& reconcile,
      const CellAdmissionReconcileValidationDTO& validation,
      AdmissionGeneration resulting_generation);
  std::uint64_t RecordCellAdmissionGenerationAdvanced(const RequestOwnerEpoch& owner_epoch,
                                                      AdmissionGeneration prior_generation,
                                                      AdmissionGeneration resulting_generation);
  std::uint64_t RecordInterruptRuleStateChanged(const IoRuleStateChangeDTO& change);
  std::uint64_t RecordActiveTcpChanged(const RequestOwnerEpoch& owner_epoch,
                                       const ActiveTcpChangeDTO& change);

  OperationJournalEventPageDTO ReadEvents(const RequestOwnerEpoch& owner_epoch,
                                          std::uint64_t after_sequence, std::size_t limit,
                                          bool operational_only = false);

 private:
  // Journal writer authority is a strict handoff under one mutex and
  // condition variable. An ordinary writer acquires only while the writer is
  // free and no Stop projection is registered as waiting; a Stop projection
  // registers under that same mutex and acquires as soon as the writer is
  // free. Once a Stop projection is waiting, no ordinary writer that does not
  // already hold the writer can precede it: it waits for exactly the one write
  // in progress.
  //
  // The Stop's wait has no wall-clock bound of its own. The write it waits for
  // is a single transaction whose lock waits are bounded by the SQLite busy
  // timeout and whose remaining cost is its own statements and FULL fsync, on
  // the same storage the Stop's own commit must then use. A wall-clock bound
  // on the handoff therefore cannot bound the projection on stalled storage
  // (the Stop's own fsync would stall identically); it could only turn
  // healthy-but-slow I/O into spurious failed attempts and, after the Kernel's
  // bounded retry, a spurious persistence-degraded latch. A genuine storage
  // failure surfaces as the in-progress write's own error, which releases the
  // writer, and then as the Stop transaction's own failure: the Kernel makes
  // one projection attempt and fails closed through its persistence latch. The wait runs only on the Stop projection worker after containment
  // was linearized, never on the Stop acceptance or StopControl lanes, and
  // event readers use their own WAL connection and never take the writer.
  class WriterLease final {
   public:
    explicit WriterLease(Impl& owner) noexcept : owner_(owner) {}
    ~WriterLease() { owner_.ReleaseWriter(); }
    WriterLease(const WriterLease&) = delete;
    WriterLease& operator=(const WriterLease&) = delete;
    WriterLease(WriterLease&&) = delete;
    WriterLease& operator=(WriterLease&&) = delete;

   private:
    Impl& owner_;
  };

  [[nodiscard]] WriterLease LockOrdinaryWriter() {
    std::unique_lock gate(writer_gate_mutex_);
    const auto may_acquire = [this] {
      return !writer_held_ && stop_projection_writers_waiting_ == 0U;
    };
    if (!may_acquire()) {
      gate.unlock();
      if (critical_section_observer_) {
        critical_section_observer_("ordinary_writer_wait");
      }
      gate.lock();
      writer_gate_changed_.wait(gate, may_acquire);
    }
    writer_held_ = true;
    return WriterLease(*this);
  }

  [[nodiscard]] WriterLease LockStopProjectionWriter() {
    std::unique_lock gate(writer_gate_mutex_);
    if (writer_held_) {
      ++stop_projection_writers_waiting_;
      gate.unlock();
      try {
        if (critical_section_observer_) {
          critical_section_observer_("stop_projection_writer_wait");
        }
      } catch (...) {
        gate.lock();
        --stop_projection_writers_waiting_;
        gate.unlock();
        writer_gate_changed_.notify_all();
        throw;
      }
      gate.lock();
      writer_gate_changed_.wait(gate, [this] { return !writer_held_; });
      --stop_projection_writers_waiting_;
    }
    writer_held_ = true;
    return WriterLease(*this);
  }

  void ReleaseWriter() noexcept {
    {
      std::lock_guard gate(writer_gate_mutex_);
      writer_held_ = false;
    }
    writer_gate_changed_.notify_all();
  }

  void RequireOpen() const {
    Require(!closed_ && database_ != nullptr, "operation_journal_closed",
            "operation journal database is closed");
  }

  // Event reads use their own read-only WAL connection and lock. They observe
  // a committed snapshot and never take the writer's process lock, so a
  // reader can neither delay nor starve a Stop projection, which waits only
  // for the one write in progress. SQLite WAL readers never block the writer.
  void OpenReader() {
    const auto result = sqlite3_open_v2(
        database_path_.string().c_str(), &reader_,
        SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX | SQLITE_OPEN_NOFOLLOW, nullptr);
    if (result != SQLITE_OK) {
      Fail("operation_journal_storage_failure",
           "open operation journal reader: " + SqliteDetail(reader_, result));
    }
    Check(reader_, sqlite3_busy_timeout(reader_, kBusyTimeoutMilliseconds),
          "configure operation journal reader busy timeout");
    Exec(reader_, "PRAGMA query_only=ON");
    Exec(reader_, "PRAGMA trusted_schema=OFF");
    int defensive_enabled = 0;
    Check(reader_, sqlite3_db_config(reader_, SQLITE_DBCONFIG_DEFENSIVE, 1, &defensive_enabled),
          "enable defensive operation journal reader");
    Require(defensive_enabled == 1,
            "operation_journal_storage_unsupported",
            "operation journal reader could not be configured");
    sqlite3_limit(reader_, SQLITE_LIMIT_LENGTH, static_cast<int>(kMaximumRecordJsonBytes));
  }

  void Configure() {
    Check(database_, sqlite3_busy_timeout(database_, kBusyTimeoutMilliseconds),
          "configure operation journal busy timeout");
    Exec(database_, "PRAGMA journal_mode=WAL");
    Exec(database_, "PRAGMA synchronous=FULL");
    Exec(database_, "PRAGMA foreign_keys=ON");
    Exec(database_, "PRAGMA trusted_schema=OFF");
    Exec(database_, "PRAGMA recursive_triggers=OFF");
    Exec(database_, "PRAGMA wal_autocheckpoint=1000");
    int defensive_enabled = 0;
    Check(database_, sqlite3_db_config(database_, SQLITE_DBCONFIG_DEFENSIVE, 1, &defensive_enabled),
          "enable defensive operation journal connection");
    Require(defensive_enabled == 1, "operation_journal_storage_unsupported",
            "SQLite defensive mode could not be enabled");
    const auto page_size = ScalarInteger(database_, "PRAGMA page_size");
    Require(page_size > 0, "operation_journal_corrupt",
            "operation journal SQLite page size is invalid");
    const auto maximum_pages =
        std::max<std::int64_t>(1, kMaximumDatabasePageImageBytes / page_size);
    Require(ScalarInteger(database_, "PRAGMA max_page_count=" + std::to_string(maximum_pages)) ==
                maximum_pages,
            "operation_journal_storage_failure",
            "operation journal SQLite page ceiling could not be configured");
    sqlite3_limit(database_, SQLITE_LIMIT_LENGTH, static_cast<int>(kMaximumRecordJsonBytes));
  }

  void InitializeOrValidate() {
    const auto lock = LockOrdinaryWriter();
    Transaction transaction(database_);
    const auto objects = SchemaObjectNames(database_);
    if (objects.empty()) {
      Require(ScalarInteger(database_, "PRAGMA application_id") == 0 &&
                  ScalarInteger(database_, "PRAGMA user_version") == 0,
              "operation_journal_storage_foreign",
              "schema-empty operation journal has a foreign identity");
      Exec(database_, kSchema);
      Exec(database_,
           "PRAGMA application_id=" + std::to_string(kOperationJournalSqliteApplicationId));
      Exec(database_, "PRAGMA user_version=" + std::to_string(kOperationJournalSchemaVersion));
      Statement metadata(database_,
                         "INSERT INTO journal_metadata(singleton,schema_id,schema_version,"
                         "cell_id,history_capacity,cell_admission_generation_high_water,"
                         "cell_claim_generation_high_water) VALUES(1,?,?,?,?,0,0)");
      metadata.Bind(1, kOperationJournalSchemaId);
      metadata.Bind(2, kOperationJournalSchemaVersion);
      metadata.Bind(3, cell_id_.value());
      metadata.Bind(4, static_cast<std::int64_t>(history_capacity_));
      Require(!metadata.Step(), "operation_journal_corrupt",
              "operation journal metadata insert returned an unexpected row");
    }
    // v19 added the takeover planning cap; v18 dropped the Stop fault latch
    // and surveillance records. Earlier journals are refused and left
    // untouched, never converted.
    Require(ScalarInteger(database_, "PRAGMA user_version") >= kOperationJournalSchemaVersion,
            "operation_journal_format_unsupported",
            "operation journal predates the v19 takeover record and is not converted");
    Require(SchemaObjectNames(database_) == ExpectedSchemaObjects(),
            "operation_journal_storage_foreign", "operation journal contains an unexpected schema");
    Require(
        ScalarInteger(database_, "PRAGMA application_id") == kOperationJournalSqliteApplicationId,
        "operation_journal_storage_foreign", "operation journal application identity is invalid");
    Require(
        ScalarInteger(database_, "PRAGMA application_id") == kOperationJournalSqliteApplicationId &&
            ScalarInteger(database_, "PRAGMA user_version") == kOperationJournalSchemaVersion,
        "operation_journal_storage_foreign", "operation journal SQLite identity is invalid");
    Statement metadata(database_,
                       "SELECT schema_id,schema_version,cell_id,history_capacity,"
                       "cell_admission_generation_high_water,cell_claim_generation_high_water "
                       "FROM journal_metadata WHERE singleton=1");
    Require(metadata.Step() && metadata.Text(0) == kOperationJournalSchemaId &&
                metadata.Integer(1) == kOperationJournalSchemaVersion &&
                metadata.Text(2) == cell_id_.value() &&
                metadata.Integer(3) == static_cast<std::int64_t>(history_capacity_) &&
                metadata.Integer(4) >= 0 && metadata.Integer(5) >= 0 && !metadata.Step(),
            "operation_journal_storage_foreign",
            "operation journal metadata is invalid or configuration changed");
    transaction.Commit();
  }

  std::uint64_t Append(const RequestOwnerEpoch& owner_epoch, std::string_view record_kind,
                       const std::optional<RequestId>& request_id,
                       const CanonicalValue& record_value, const OperationStateDTO& state,
                       std::int64_t occurred_at_ns, bool effect_pending,
                       std::string_view disposition,
                       std::span<const OperationId> protected_operation_ids = {});

  std::uint64_t AppendStopAcceptance(const RequestOwnerEpoch& owner_epoch,
                                     const EmergencyStopAcceptanceRecordDTO& acceptance,
                                     const CanonicalValue& record_value,
                                     const StopAcceptedDTO& accepted,
                                     const EmergencyStopAffectedStateSnapshotDTO& affected_state);

  std::uint64_t AppendStopTerminal(const RequestOwnerEpoch& owner_epoch,
                                   const StopJournalTerminalProjectionDTO& projection,
                                   const std::string& payload_json);

  std::filesystem::path database_path_;
  CellId cell_id_;
  std::size_t history_capacity_;
  std::function<std::string()> wall_time_text_;
  std::function<void(std::string_view)> critical_section_observer_;
  std::mutex writer_gate_mutex_;
  std::condition_variable writer_gate_changed_;
  bool writer_held_{};
  std::size_t stop_projection_writers_waiting_{};
  sqlite3* database_{};
  bool closed_{};
  std::mutex reader_mutex_;
  sqlite3* reader_{};
};

std::uint64_t SqliteOperationJournal::Impl::Append(
    const RequestOwnerEpoch& owner_epoch, std::string_view record_kind,
    const std::optional<RequestId>& request_id, const CanonicalValue& record_value,
    const OperationStateDTO& state, std::int64_t occurred_at_ns, bool effect_pending,
    std::string_view disposition, const std::span<const OperationId> protected_operation_ids) {
  Require(!owner_epoch.empty() && !record_kind.empty() &&
              (!request_id.has_value() || !request_id->empty()) && occurred_at_ns >= 0,
          "operation_journal_record_invalid", "operation journal record identity is invalid");
  RequireState(state);
  Require(protected_operation_ids.empty() ||
              (record_kind == "operation_accepted" &&
               std::all_of(protected_operation_ids.begin(), protected_operation_ids.end(),
                           [](const OperationId& id) { return !id.empty(); }) &&
               std::is_sorted(protected_operation_ids.begin(), protected_operation_ids.end()) &&
               std::adjacent_find(protected_operation_ids.begin(), protected_operation_ids.end()) ==
                   protected_operation_ids.end() &&
               std::binary_search(protected_operation_ids.begin(), protected_operation_ids.end(),
                                  state.operation_id)),
          "operation_journal_record_invalid", "protected operation admission inventory is invalid");
  const auto payload_json = content_identity::canonical_json(record_value);
  Require(payload_json.size() <= kMaximumRecordJsonBytes, "operation_journal_record_too_large",
          "operation journal record exceeds the fixed payload bound");
  const auto payload_digest = content_identity::sha256_digest(payload_json);

  const auto lock = LockOrdinaryWriter();
  RequireOpen();
  const auto occurred_at_wall = wall_time_text_();
  ValidateWallTimeText(occurred_at_wall);
  Transaction transaction(database_);
  if (critical_section_observer_) {
    critical_section_observer_("ordinary_write");
  }
  Statement active(database_,
                   "SELECT request_capacity FROM owner_epochs "
                   "WHERE owner_epoch=? AND active=1");
  active.Bind(1, owner_epoch.value());
  Require(active.Step(), "operation_journal_owner_epoch_stale",
          "operation journal write names a non-active owner epoch");
  const auto request_capacity = active.Integer(0);
  Require(request_capacity > 0 && !active.Step(), "operation_journal_corrupt",
          "active operation journal owner epoch is invalid");
  if (!protected_operation_ids.empty()) {
    const auto retained_capacity =
        std::min(history_capacity_, static_cast<std::size_t>(request_capacity));
    Require(protected_operation_ids.size() <= retained_capacity, "operation_capacity_full",
            "active or unresolved operations occupy the configured operation capacity");
    const auto historical_budget = retained_capacity - protected_operation_ids.size();
    std::vector<OperationId> compact;
    std::size_t retained_historical = 0U;
    Statement snapshot_rows(database_,
                            "SELECT operation_id FROM operation_snapshots "
                            "ORDER BY snapshot_sequence DESC");
    while (snapshot_rows.Step()) {
      const OperationId operation_id(snapshot_rows.Text(0));
      if (std::binary_search(protected_operation_ids.begin(), protected_operation_ids.end(),
                             operation_id)) {
        continue;
      }
      if (retained_historical++ >= historical_budget) {
        compact.push_back(operation_id);
      }
    }
    for (const auto& operation_id : compact) {
      Statement remove_snapshot(database_, "DELETE FROM operation_snapshots WHERE operation_id=?");
      remove_snapshot.Bind(1, operation_id.value());
      Require(!remove_snapshot.Step(), "operation_journal_corrupt",
              "resolved operation snapshot compaction failed");
      Statement remove_records(database_, "DELETE FROM journal_records WHERE operation_id=?");
      remove_records.Bind(1, operation_id.value());
      Require(!remove_records.Step(), "operation_journal_corrupt",
              "resolved operation record compaction failed");
    }
  }
  if (request_id.has_value()) {
    Statement existing(database_,
                       "SELECT record_kind,payload_digest FROM journal_records "
                       "WHERE owner_epoch=? AND request_id=? "
                       "AND record_kind<>'stop_acceptance_projected'");
    existing.Bind(1, owner_epoch.value());
    existing.Bind(2, request_id->value());
    Require(!existing.Step(), "request_id_conflict",
            "operation journal request identity is already recorded");
    Statement request_count(database_,
                            "SELECT COUNT(DISTINCT operation_id) FROM journal_records "
                            "WHERE owner_epoch=? AND request_id IS NOT NULL "
                            "AND record_kind<>'stop_acceptance_projected' "
                            "AND (operation_id IS NULL OR operation_id<>?)");
    request_count.Bind(1, owner_epoch.value());
    request_count.Bind(2, state.operation_id.value());
    Require(request_count.Step(), "operation_journal_corrupt",
            "operation journal resident-operation count returned no row");
    const auto count = request_count.Integer(0);
    Require(!request_count.Step() && count >= 0, "operation_journal_corrupt",
            "operation journal resident-operation count is invalid");
    Require(count < request_capacity, "request_ledger_full",
            "operation journal request ledger reached its fixed bound");
  }

  Statement insert(database_,
                   "INSERT INTO journal_records(owner_epoch,record_kind,request_id,"
                   "operation_id,operation_revision,occurred_at_ns,occurred_at_wall,"
                   "effect_pending,payload_json,payload_digest) VALUES(?,?,?,?,?,?,?,?,?,?)");
  insert.Bind(1, owner_epoch.value());
  insert.Bind(2, record_kind);
  if (request_id.has_value()) {
    insert.Bind(3, request_id->value());
  } else {
    insert.BindNull(3);
  }
  insert.Bind(4, state.operation_id.value());
  insert.Bind(5, static_cast<std::int64_t>(state.operation_revision.value()));
  insert.Bind(6, occurred_at_ns);
  insert.Bind(7, occurred_at_wall);
  insert.Bind(8, static_cast<std::int64_t>(effect_pending ? 1 : 0));
  insert.Bind(9, payload_json);
  insert.Bind(10, payload_digest);
  Require(!insert.Step(), "operation_journal_corrupt",
          "operation journal insert returned an unexpected row");
  const auto sequence = sqlite3_last_insert_rowid(database_);
  const auto bounded_sequence = PositiveIdentifier(sequence, "operation journal sequence");
  StoreOperationSnapshot(database_, owner_epoch, state, bounded_sequence, disposition,
                         payload_digest);

  // The live journal stores one initial acceptance, the bounded preparation
  // provenance, and the newest complete state of an operation, not an
  // ever-growing command receipt history.
  // The state anchor advances unless this is a delayed lower-revision fact.
  // Preserve that current anchor plus the newest event, not a lifetime list.
  // Unresolved physical work remains represented by the snapshot itself.
  Statement coalesce(database_,
                     "DELETE FROM journal_records WHERE operation_id=? AND sequence<>? "
                     "AND record_kind IN ('control_accepted','runtime_scale_accepted',"
                     "'state_observed') "
                     "AND sequence NOT IN (SELECT snapshot_sequence FROM operation_snapshots)");
  coalesce.Bind(1, state.operation_id.value());
  coalesce.Bind(2, static_cast<std::int64_t>(sequence));
  Require(!coalesce.Step(), "operation_journal_corrupt",
          "operation journal current-state coalescing returned an unexpected row");
  transaction.Commit();
  return bounded_sequence;
}

std::uint64_t SqliteOperationJournal::Impl::AppendStopAcceptance(
    const RequestOwnerEpoch& owner_epoch, const EmergencyStopAcceptanceRecordDTO& acceptance,
    const CanonicalValue& record_value, const StopAcceptedDTO& accepted,
    const EmergencyStopAffectedStateSnapshotDTO& affected_state) {
  const auto payload_json = content_identity::canonical_json(record_value);
  Require(payload_json.size() <= kMaximumRecordJsonBytes, "operation_journal_record_too_large",
          "Stop acceptance projection exceeds the fixed payload bound");
  const auto payload_digest = content_identity::sha256_digest(payload_json);

  const auto lock = LockStopProjectionWriter();
  RequireOpen();
  const auto occurred_at_wall = wall_time_text_();
  ValidateWallTimeText(occurred_at_wall);
  Transaction transaction(database_);
  Statement active(database_, "SELECT COUNT(*) FROM owner_epochs WHERE owner_epoch=? AND active=1");
  active.Bind(1, owner_epoch.value());
  Require(active.Step() && active.Integer(0) == 1 && !active.Step(),
          "operation_journal_owner_epoch_stale",
          "Stop acceptance projection names a non-active owner epoch");

  // A request that joined a resident physical Stop is an additional request
  // receipt for that Stop, not a new capture. The creating acceptance owns the
  // Stop's recovery projection and the operation captures it carries; later
  // state and terminal records advance them. A joined receipt carries the same
  // operation captures (equal journal capture order) inside different record
  // bytes, so it must neither re-anchor operation snapshots nor claim the
  // Stop's single recovery projection row. Its projection may run before or
  // after the creator's: only the physical transaction identity is joined.
  const bool joined_receipt =
      acceptance.acceptance_path == EmergencyStopAcceptancePathDTO::kJoinedResident;
  const auto require_joined_transaction = [&] {
    Statement creator(database_, kStopProjectionAcceptedPayloadSelectSql);
    creator.Bind(1, accepted.stop_id.value());
    creator.Bind(2, static_cast<std::int64_t>(accepted.stop_generation.value()));
    if (!creator.Step()) return;
    auto decoded = DecodeStopJournalAcceptanceProjection(VerifiedAcceptedPayload(creator, 0));
    Require(decoded.ok() && !creator.Step() &&
                decoded.value().accepted.captured_scope_digest == accepted.captured_scope_digest,
            "operation_journal_stop_projection_conflict",
            "joined Stop acceptance does not name its resident physical Stop");
  };

  // Projection retry is idempotent by exact canonical payload. Different
  // accepted records may legitimately share a request ID after conservative
  // conflict promotion, so request identity alone is not a uniqueness key.
  Statement existing(database_,
                     "SELECT sequence,payload_json FROM journal_records "
                     "WHERE owner_epoch=? AND record_kind='stop_acceptance_projected' "
                     "AND request_id=? AND stop_id=? AND stop_generation=?");
  existing.Bind(1, owner_epoch.value());
  existing.Bind(2, acceptance.request_id.value());
  existing.Bind(3, acceptance.stop_id.value());
  existing.Bind(4, static_cast<std::int64_t>(acceptance.stop_generation.value()));
  if (existing.Step()) {
    const auto sequence = PositiveIdentifier(existing.Integer(0), "operation journal sequence");
    Require(existing.Text(1) == payload_json && !existing.Step(),
            "operation_journal_stop_projection_conflict",
            "Stop acceptance identity names different canonical projection bytes");
    if (joined_receipt) {
      require_joined_transaction();
      transaction.Commit();
      return sequence;
    }
    const auto acceptance_projection =
        EncodeStopJournalAcceptanceProjection(StopJournalAcceptanceProjectionDTO{accepted});
    Require(acceptance_projection.ok(), "operation_journal_record_invalid",
            "Stop acceptance recovery projection is invalid");
    Statement existing_projection(database_, kStopProjectionAcceptanceSelectSql);
    existing_projection.Bind(1, accepted.stop_id.value());
    existing_projection.Bind(2, static_cast<std::int64_t>(accepted.stop_generation.value()));
    Require(existing_projection.Step() &&
                PositiveIdentifier(existing_projection.Integer(0), "Stop generation") ==
                    accepted.stop_generation.value() &&
                PositiveIdentifier(existing_projection.Integer(1), "operation journal sequence") ==
                    sequence &&
                VerifiedAcceptedPayload(existing_projection, 2) == acceptance_projection.value() &&
                !existing_projection.Step(),
            "operation_journal_stop_projection_conflict",
            "Stop acceptance replay conflicts with its durable recovery projection");
    transaction.Commit();
    return sequence;
  }

  Statement insert(database_,
                   "INSERT INTO journal_records(owner_epoch,record_kind,request_id,stop_id,"
                   "stop_generation,"
                   "operation_id,operation_revision,occurred_at_ns,occurred_at_wall,"
                   "effect_pending,payload_json,payload_digest) "
                   "VALUES(?,'stop_acceptance_projected',?,?,?,NULL,NULL,?,?,?,?,?)");
  insert.Bind(1, owner_epoch.value());
  insert.Bind(2, acceptance.request_id.value());
  insert.Bind(3, acceptance.stop_id.value());
  insert.Bind(4, static_cast<std::int64_t>(acceptance.stop_generation.value()));
  insert.Bind(5, acceptance.accepted_at_ns);
  insert.Bind(6, occurred_at_wall);
  insert.Bind(7, static_cast<std::int64_t>(
                     acceptance.entry_state == EmergencyStopEntryStateDTO::kActive ? 1 : 0));
  insert.Bind(8, payload_json);
  insert.Bind(9, payload_digest);
  Require(!insert.Step(), "operation_journal_corrupt",
          "Stop acceptance journal insert returned an unexpected row");
  const auto sequence =
      PositiveIdentifier(sqlite3_last_insert_rowid(database_), "operation journal sequence");
  if (joined_receipt) {
    require_joined_transaction();
    transaction.Commit();
    return sequence;
  }
  for (const auto& state : affected_state.operations) {
    StoreOperationSnapshotMonotonic(database_, owner_epoch, state, sequence, payload_digest);
  }
  // The index row keeps only the positional `accepted` (v12); the affected
  // state stays in the journal row written above.
  const auto acceptance_projection =
      EncodeStopJournalAcceptanceProjection(StopJournalAcceptanceProjectionDTO{accepted});
  Require(acceptance_projection.ok(), "operation_journal_record_invalid",
          "Stop acceptance recovery projection is invalid");
  Statement insert_projection(database_, kStopProjectionInsertSql);
  insert_projection.Bind(1, accepted.stop_id.value());
  insert_projection.Bind(2, static_cast<std::int64_t>(accepted.stop_generation.value()));
  insert_projection.Bind(3, static_cast<std::int64_t>(sequence));
  insert_projection.Bind(4, acceptance_projection.value());
  insert_projection.Bind(5, content_identity::sha256_digest(acceptance_projection.value()));
  Require(!insert_projection.Step(), "operation_journal_corrupt",
          "Stop acceptance recovery projection insert returned an unexpected row");
  transaction.Commit();
  return sequence;
}

std::uint64_t SqliteOperationJournal::Impl::AppendStopTerminal(
    const RequestOwnerEpoch& owner_epoch, const StopJournalTerminalProjectionDTO& projection,
    const std::string& payload_json) {
  Require(payload_json.size() <= kMaximumRecordJsonBytes, "operation_journal_record_too_large",
          "Stop terminal projection exceeds the fixed payload bound");
  const auto payload_digest = content_identity::sha256_digest(payload_json);

  const auto lock = LockStopProjectionWriter();
  RequireOpen();
  const auto occurred_at_wall = wall_time_text_();
  ValidateWallTimeText(occurred_at_wall);
  Transaction transaction(database_);
  Statement active(database_, "SELECT COUNT(*) FROM owner_epochs WHERE owner_epoch=? AND active=1");
  active.Bind(1, owner_epoch.value());
  Require(active.Step() && active.Integer(0) == 1 && !active.Step(),
          "operation_journal_owner_epoch_stale",
          "Stop terminal projection names a non-active owner epoch");

  Statement acceptance(database_, kStopProjectionTerminalSelectSql);
  acceptance.Bind(1, projection.certificate.stop_id.value());
  acceptance.Bind(2, static_cast<std::int64_t>(projection.certificate.stop_generation.value()));
  Require(acceptance.Step(), "operation_journal_stop_acceptance_missing",
          "Stop terminal projection has no durable acceptance predecessor");
  if (!acceptance.IsNull(0)) {
    const auto sequence = PositiveIdentifier(acceptance.Integer(0), "operation journal sequence");
    Require(acceptance.Text(1) == payload_json && !acceptance.Step(),
            "operation_journal_stop_projection_conflict",
            "Stop terminal identity names different canonical projection bytes");
    transaction.Commit();
    return sequence;
  }
  Require(!acceptance.Step(), "operation_journal_corrupt",
          "Stop terminal acceptance lookup returned duplicate rows");

  for (const auto& predecessor : projection.certificate.predecessor_certificates) {
    Statement predecessor_record(database_, kStopProjectionTerminalSelectSql);
    predecessor_record.Bind(1, predecessor.stop_id.value());
    predecessor_record.Bind(2, static_cast<std::int64_t>(predecessor.stop_generation.value()));
    Require(predecessor_record.Step() && !predecessor_record.IsNull(1),
            "operation_journal_stop_predecessor_unresolved",
            "Stop terminal predecessor has no durable terminal certificate truth");
    auto decoded_predecessor = DecodeStopJournalTerminalProjection(predecessor_record.Text(1));
    Require(decoded_predecessor.ok() &&
                decoded_predecessor.value().certificate.stop_certificate_id ==
                    predecessor.stop_certificate_id &&
                decoded_predecessor.value().certificate.stop_certificate_revision ==
                    predecessor.stop_certificate_revision &&
                !predecessor_record.Step(),
            "operation_journal_stop_predecessor_unresolved",
            "Stop terminal predecessor reference does not match durable certificate truth");
  }

  Statement insert(database_,
                   "INSERT INTO journal_records(owner_epoch,record_kind,request_id,stop_id,"
                   "stop_generation,"
                   "operation_id,operation_revision,occurred_at_ns,occurred_at_wall,"
                   "effect_pending,payload_json,payload_digest) "
                   "VALUES(?,'stop_terminal_projected',NULL,?,?,NULL,NULL,?,?,?,?,?)");
  insert.Bind(1, owner_epoch.value());
  insert.Bind(2, projection.certificate.stop_id.value());
  insert.Bind(3, static_cast<std::int64_t>(projection.certificate.stop_generation.value()));
  insert.Bind(4, projection.certificate.completed_at_ns);
  insert.Bind(5, occurred_at_wall);
  insert.Bind(6, static_cast<std::int64_t>(0));
  insert.Bind(7, payload_json);
  insert.Bind(8, payload_digest);
  Require(!insert.Step(), "operation_journal_corrupt",
          "Stop terminal journal insert returned an unexpected row");
  const auto sequence =
      PositiveIdentifier(sqlite3_last_insert_rowid(database_), "operation journal sequence");
  Statement update(database_, kStopProjectionTerminalUpdateSql);
  update.Bind(1, static_cast<std::int64_t>(sequence));
  update.Bind(2, projection.certificate.stop_id.value());
  update.Bind(3, static_cast<std::int64_t>(projection.certificate.stop_generation.value()));
  Require(!update.Step() && sqlite3_changes(database_) == 1, "operation_journal_corrupt",
          "Stop terminal durable projection update did not affect exactly one row");
  transaction.Commit();
  return sequence;
}

OperationJournalEventPageDTO SqliteOperationJournal::Impl::ReadEvents(
    const RequestOwnerEpoch& owner_epoch, const std::uint64_t after_sequence,
    const std::size_t limit, const bool operational_only) {
  Require(!owner_epoch.empty() && after_sequence <= kMaximumSafeInteger && limit > 0U &&
              limit <= kMaximumOperationJournalEventPage,
          "operation_journal_event_query_invalid",
          "operation journal event query exceeds its identity or page bound");

  std::lock_guard lock(reader_mutex_);
  Require(reader_ != nullptr, "operation_journal_closed", "operation journal database is closed");
  // One committed snapshot covers the owner check and the page.
  ReadSnapshot snapshot(reader_);
  Statement active(reader_, "SELECT COUNT(*) FROM owner_epochs WHERE owner_epoch=? AND active=1");
  active.Bind(1, owner_epoch.value());
  Require(active.Step() && active.Integer(0) == 1 && !active.Step(),
          "operation_journal_owner_epoch_stale",
          "operation event query names a non-active owner epoch");
  if (critical_section_observer_) {
    critical_section_observer_("read_events");
  }

  Statement query(reader_,
                  operational_only
                      ? "SELECT sequence,record_kind,request_id,operation_id,"
                        "operation_revision,occurred_at_ns,occurred_at_wall,effect_pending,"
                        "payload_json,payload_digest FROM journal_records "
                        "WHERE owner_epoch=? AND sequence>? AND record_kind IN "
                        "('operation_accepted','control_accepted','stop_acceptance_projected',"
                        "'stop_terminal_projected',"
                        "'runtime_scale_accepted','operation_prepared',"
                        "'state_observed','interrupt_rule_state_changed',"
                        "'active_tcp_changed','host_replaced') "
                        "ORDER BY sequence LIMIT ?"
                      : "SELECT sequence,record_kind,request_id,operation_id,"
                        "operation_revision,occurred_at_ns,occurred_at_wall,effect_pending,"
                        "payload_json,payload_digest FROM journal_records "
                        "WHERE owner_epoch=? AND sequence>? ORDER BY sequence LIMIT ?");
  query.Bind(1, owner_epoch.value());
  query.Bind(2, static_cast<std::int64_t>(after_sequence));
  query.Bind(3, static_cast<std::int64_t>(limit + 1U));
  std::vector<OperationJournalEventDTO> events;
  events.reserve(limit + 1U);
  while (query.Step()) {
    const auto sequence = PositiveIdentifier(query.Integer(0), "operation journal event sequence");
    const auto record_kind = query.Text(1);
    const auto request_id = query.IsNull(2) ? std::optional<RequestId>{}
                                            : std::optional<RequestId>(RequestId(query.Text(2)));
    const bool has_operation_id = !query.IsNull(3);
    const bool has_operation_revision = !query.IsNull(4);
    Require(has_operation_id == has_operation_revision, "operation_journal_corrupt",
            "operation journal event has an incomplete operation identity");
    const auto operation_id =
        has_operation_id ? std::optional<OperationId>(OperationId(query.Text(3))) : std::nullopt;
    const auto operation_revision =
        has_operation_revision ? std::optional<OperationRevision>(OperationRevision(
                                     PositiveIdentifier(query.Integer(4), "operation revision")))
                               : std::nullopt;
    const auto occurred_at_ns = query.Integer(5);
    const auto occurred_at_wall = query.Text(6);
    const auto effect_pending_value = query.Integer(7);
    const auto payload_json = query.Text(8);
    const auto payload_digest = query.Text(9);
    Require(sequence > after_sequence && !record_kind.empty() &&
                (!operation_id.has_value() || !operation_id->empty()) && occurred_at_ns >= 0 &&
                (effect_pending_value == 0 || effect_pending_value == 1) &&
                payload_json.size() <= kMaximumRecordJsonBytes &&
                content_identity::sha256_digest(payload_json) == payload_digest,
            "operation_journal_corrupt", "operation journal event record is invalid or corrupt");
    ValidateWallTimeText(occurred_at_wall);
    events.push_back(OperationJournalEventDTO{
        sequence, owner_epoch, std::move(record_kind), request_id, operation_id, operation_revision,
        occurred_at_ns, std::move(occurred_at_wall), effect_pending_value == 1,
        std::move(payload_json), std::move(payload_digest)});
  }
  const bool has_more = events.size() > limit;
  if (has_more) {
    events.erase(events.begin() + static_cast<std::ptrdiff_t>(limit), events.end());
  }
  const auto next_after_sequence = events.empty() ? after_sequence : events.back().sequence;
  return OperationJournalEventPageDTO{owner_epoch, std::move(events), next_after_sequence,
                                      has_more};
}

OperationJournalRecoveryDTO SqliteOperationJournal::Impl::BeginOwnerEpoch(
    const RequestOwnerEpoch& owner_epoch, std::int64_t started_at_ns, std::size_t request_capacity,
    const std::vector<ResourceId>& resource_inventory) {
  Require(!owner_epoch.empty() && started_at_ns >= 0 && request_capacity > 0U &&
              request_capacity <= kMaximumRequestCapacity &&
              resource_inventory.size() <= kMaxKernelResourceSlots,
          "operation_journal_owner_epoch_invalid",
          "operation journal owner epoch admission is invalid");
  const std::set<ResourceId> inventory(resource_inventory.begin(), resource_inventory.end());
  Require(inventory.size() == resource_inventory.size() &&
              std::none_of(resource_inventory.begin(), resource_inventory.end(),
                           [](const auto& resource) { return resource.empty(); }),
          "operation_journal_owner_epoch_invalid",
          "operation journal resource inventory is invalid");

  const auto lock = LockOrdinaryWriter();
  RequireOpen();
  Transaction transaction(database_);
  Statement reused(database_, "SELECT COUNT(*) FROM owner_epochs WHERE owner_epoch=?");
  reused.Bind(1, owner_epoch.value());
  Require(reused.Step() && reused.Integer(0) == 0 && !reused.Step(),
          "operation_journal_owner_epoch_reused", "operation journal owner epoch was already used");

  Statement prior_snapshots(database_,
                            "SELECT operation_id,owner_epoch,disposition FROM operation_snapshots "
                            "ORDER BY snapshot_sequence");
  struct PriorSnapshot final {
    OperationStateDTO state;
    RequestOwnerEpoch owner_epoch;
    std::string disposition;
  };
  std::vector<PriorSnapshot> prior_states;
  while (prior_snapshots.Step()) {
    const OperationId operation_id(prior_snapshots.Text(0));
    prior_states.push_back(PriorSnapshot{LoadOperationState(database_, operation_id),
                                         RequestOwnerEpoch(prior_snapshots.Text(1)),
                                         prior_snapshots.Text(2)});
  }

  Statement prior_active(database_,
                         "SELECT owner_epoch,close_sequence FROM owner_epochs WHERE active=1");
  std::optional<RequestOwnerEpoch> replaced_unclosed_owner_epoch;
  if (prior_active.Step()) {
    if (prior_active.IsNull(1)) {
      replaced_unclosed_owner_epoch = RequestOwnerEpoch(prior_active.Text(0));
    }
    Require(!prior_active.Step(), "operation_journal_corrupt",
            "operation journal contains multiple active owner epochs");
  }
  Statement prior_epoch(database_,
                        "SELECT close_sequence FROM owner_epochs ORDER BY epoch_sequence DESC "
                        "LIMIT 1");
  const bool prior_epoch_clean = prior_epoch.Step() && !prior_epoch.IsNull(0);
  Require(!prior_epoch.Step(), "operation_journal_corrupt",
          "prior owner epoch query returned multiple rows");
  Exec(database_, "UPDATE owner_epochs SET active=0 WHERE active=1");
  Statement insert_epoch(database_,
                         "INSERT INTO owner_epochs(owner_epoch,started_at_ns,request_capacity,"
                         "active) VALUES(?,?,?,1)");
  insert_epoch.Bind(1, owner_epoch.value());
  insert_epoch.Bind(2, started_at_ns);
  insert_epoch.Bind(3, static_cast<std::int64_t>(request_capacity));
  Require(!insert_epoch.Step(), "operation_journal_corrupt",
          "operation owner epoch insert returned an unexpected row");

  Statement generation_high_water(
      database_,
      "SELECT cell_admission_generation_high_water,"
      "cell_claim_generation_high_water FROM journal_metadata WHERE singleton=1");
  Require(generation_high_water.Step(), "operation_journal_corrupt",
          "cell-admission generation high-water is missing");
  const auto prior_generation = generation_high_water.Integer(0);
  const auto prior_claim_generation = generation_high_water.Integer(1);
  Require(prior_generation >= 0 &&
              static_cast<std::uint64_t>(prior_generation) < kMaximumSafeInteger &&
              prior_claim_generation >= 0 &&
              static_cast<std::uint64_t>(prior_claim_generation) < kMaximumSafeInteger &&
              !generation_high_water.Step(),
          "operation_journal_corrupt", "cell-admission generation high-water is invalid");
  const auto replacement_generation = std::max<std::int64_t>(1, prior_generation + 1);
  Statement advance_generation(
      database_,
      "UPDATE journal_metadata SET cell_admission_generation_high_water=? WHERE singleton=1");
  advance_generation.Bind(1, replacement_generation);
  Require(!advance_generation.Step() && sqlite3_changes(database_) == 1,
          "operation_journal_corrupt", "cell-admission replacement generation did not advance");

  if (prior_epoch_clean) {
    Statement retire_stale_claims(
        database_,
        "DELETE FROM cell_admission_claims WHERE claim_generation IN ("
        "SELECT membership.claim_generation "
        "FROM cell_admission_request_membership membership "
        "WHERE NOT EXISTS (SELECT 1 FROM cell_admission_current_owners current "
        "WHERE current.owner_role=membership.owner_role "
        "AND current.owner_epoch=membership.owner_epoch)) "
        "AND release_owner_role IS NOT NULL");
    Require(!retire_stale_claims.Step(), "operation_journal_corrupt",
            "stale cell-admission claim retirement failed");
    Statement retire_stale_membership(
        database_,
        "DELETE FROM cell_admission_request_membership "
        "WHERE NOT EXISTS (SELECT 1 FROM cell_admission_current_owners current "
        "WHERE current.owner_role=cell_admission_request_membership.owner_role "
        "AND current.owner_epoch=cell_admission_request_membership.owner_epoch) "
        "AND claim_generation NOT IN ("
        "SELECT claim_generation FROM cell_admission_claims)");
    Require(!retire_stale_membership.Step(), "operation_journal_corrupt",
            "stale cell-admission membership retirement failed");
    Statement compact_reconciliations(
        database_,
        "DELETE FROM cell_admission_reconciliations "
        "WHERE reconcile_sequence<>("
        "SELECT COALESCE(MAX(reconcile_sequence),0) "
        "FROM cell_admission_reconciliations) "
        "AND NOT EXISTS (SELECT 1 FROM cell_admission_reconcile_settlements settlement "
        "WHERE settlement.reconcile_sequence="
        "cell_admission_reconciliations.reconcile_sequence)");
    Require(!compact_reconciliations.Step(), "operation_journal_corrupt",
            "settled cell-admission reconciliation compaction failed");
  }
  Statement invalid_reconciled(
      database_,
      "SELECT COUNT(*) FROM cell_admission_claims WHERE reconciled_sequence IS NOT NULL "
      "AND reconciled_sequence NOT IN "
      "(SELECT reconcile_sequence FROM cell_admission_reconciliations)");
  Require(
      invalid_reconciled.Step() && invalid_reconciled.Integer(0) == 0 && !invalid_reconciled.Step(),
      "operation_journal_corrupt",
      "cell-admission settled claim lacks durable reconciliation evidence");

  Statement tombstone(database_,
                      "INSERT OR IGNORE INTO request_tombstones("
                      "owner_epoch,request_id,record_kind,operation_id,payload_digest) "
                      "SELECT owner_epoch,request_id,record_kind,operation_id,payload_digest "
                      "FROM journal_records WHERE owner_epoch<>? AND request_id IS NOT NULL "
                      "AND record_kind<>'stop_acceptance_projected' "
                      "ORDER BY sequence");
  tombstone.Bind(1, owner_epoch.value());
  Require(!tombstone.Step(), "operation_journal_corrupt",
          "operation request tombstone compaction returned an unexpected row");

  std::set<ResourceId> unreconciled;
  if (replaced_unclosed_owner_epoch.has_value()) {
    // An emergency Stop may have linearized after the predecessor's last
    // durable byte.  No stored row can narrow that unknown scope, so owner
    // replacement without the explicit clean-close marker fences the complete
    // caller-supplied current inventory.
    unreconciled.insert(inventory.begin(), inventory.end());
  }
  for (auto& prior_snapshot : prior_states) {
    auto& state = prior_snapshot.state;
    if (prior_snapshot.disposition == "orphaned_by_host_replacement") {
      for (const auto& participant : state.participants) {
        for (const auto& resource : participant.resources) {
          if (inventory.contains(resource)) {
            unreconciled.insert(resource);
          }
        }
      }
      continue;
    }
    if (IsTerminal(state.state)) {
      Statement mark_terminal(database_,
                              "UPDATE operation_snapshots SET disposition='prior_terminal' "
                              "WHERE operation_id=?");
      mark_terminal.Bind(1, state.operation_id.value());
      Require(!mark_terminal.Step(), "operation_journal_corrupt",
              "terminal operation disposition update returned an unexpected row");
      continue;
    }

    const ErrorDTO replacement_error{
        "execution_host_replaced",
        "the execution host was replaced before terminal operation evidence",
        {{"new_owner_epoch", owner_epoch.value()},
         {"operation_id", state.operation_id.value()},
         {"previous_owner_epoch", prior_snapshot.owner_epoch.value()},
         {"reason", "unreconciled_external_or_orphan_goal"}}};
    state.state = OperationStateKind::kInterventionRequired;
    if (state.journal_capture_order != 0U) {
      Require(state.journal_capture_order < static_cast<std::uint64_t>(INT64_MAX),
              "operation_journal_record_invalid", "operation capture order exhausted");
      ++state.journal_capture_order;
    }
    state.updated_at_ns = started_at_ns;
    for (auto& participant : state.participants) {
      for (const auto& resource : participant.resources) {
        if (inventory.contains(resource)) {
          unreconciled.insert(resource);
        }
      }
      participant.state = OperationStateKind::kInterventionRequired;
      participant.terminal_error = replacement_error;
    }
    const auto value = RecordValue(
        "host_replaced", owner_epoch,
        CanonicalValue(CanonicalValue::Object{
            {"new_owner_epoch", CanonicalValue(owner_epoch.value())},
            {"previous_owner_epoch", CanonicalValue(prior_snapshot.owner_epoch.value())},
            {"transition_reason", CanonicalValue("execution_host_replaced")},
        }),
        CanonicalValue(nullptr), state, nullptr, false);
    const auto payload_json = content_identity::canonical_json(value);
    Require(payload_json.size() <= kMaximumRecordJsonBytes, "operation_journal_record_too_large",
            "operation host-replacement record exceeds the payload bound");
    const auto occurred_at_wall = wall_time_text_();
    ValidateWallTimeText(occurred_at_wall);
    Statement insert_record(
        database_,
        "INSERT INTO journal_records(owner_epoch,record_kind,request_id,"
        "operation_id,operation_revision,occurred_at_ns,occurred_at_wall,effect_pending,"
        "payload_json,payload_digest) VALUES(?,'host_replaced',NULL,?,?,?,?,?,?,?)");
    insert_record.Bind(1, owner_epoch.value());
    insert_record.Bind(2, state.operation_id.value());
    insert_record.Bind(3, static_cast<std::int64_t>(state.operation_revision.value()));
    insert_record.Bind(4, state.updated_at_ns);
    insert_record.Bind(5, occurred_at_wall);
    insert_record.Bind(6, static_cast<std::int64_t>(0));
    insert_record.Bind(7, payload_json);
    insert_record.Bind(8, content_identity::sha256_digest(payload_json));
    Require(!insert_record.Step(), "operation_journal_corrupt",
            "host-replacement journal insert returned an unexpected row");
    const auto sequence =
        PositiveIdentifier(sqlite3_last_insert_rowid(database_), "operation journal sequence");
    StoreOperationSnapshot(database_, owner_epoch, state, sequence, "orphaned_by_host_replacement",
                           content_identity::sha256_digest(payload_json));
  }

  Statement orphan_count(database_,
                         "SELECT COUNT(*) FROM operation_snapshots "
                         "WHERE disposition='orphaned_by_host_replacement'");
  Require(orphan_count.Step(), "operation_journal_corrupt",
          "operation orphan count returned no row");
  const auto unresolved_count = orphan_count.Integer(0);
  Require(!orphan_count.Step() && unresolved_count >= 0, "operation_journal_corrupt",
          "operation orphan count is invalid");
  Require(static_cast<std::uint64_t>(unresolved_count) <= history_capacity_,
          "operation_journal_history_full",
          "unresolved operation evidence exceeds the fixed history bound");
  const auto resolved_history_capacity =
      history_capacity_ - static_cast<std::size_t>(unresolved_count);
  Statement prune_snapshots(database_,
                            "DELETE FROM operation_snapshots WHERE operation_id IN ("
                            "SELECT operation_id FROM operation_snapshots "
                            "WHERE disposition<>'orphaned_by_host_replacement' "
                            "ORDER BY snapshot_sequence DESC LIMIT -1 OFFSET ?)");
  prune_snapshots.Bind(1, static_cast<std::int64_t>(resolved_history_capacity));
  Require(!prune_snapshots.Step(), "operation_journal_corrupt",
          "operation snapshot pruning returned an unexpected row");
  Statement prune_tombstones(database_,
                             "DELETE FROM request_tombstones WHERE tombstone_sequence IN ("
                             "SELECT tombstone_sequence FROM request_tombstones "
                             "ORDER BY tombstone_sequence DESC LIMIT -1 OFFSET ?)");
  prune_tombstones.Bind(1, static_cast<std::int64_t>(history_capacity_));
  Require(!prune_tombstones.Step(), "operation_journal_corrupt",
          "operation tombstone pruning returned an unexpected row");
  // Stop truth is bounded by eviction, never refusal (recovery hands the
  // Kernel at most kMaximumRecoveredStops, its resident Stop history bound).
  // Every Stop here belongs to an earlier owner epoch. Evict oldest first,
  // terminal Stops before nonterminal ones, and never:
  //  - a nonterminal Stop of the latest prior epoch: it is the unknown
  //    outcome this recovery must latch (an older one was already latched by
  //    a later epoch, which could not close cleanly and is latched again);
  //  - the Stop with the highest generation, which keeps the recovered cell
  //    Stop generation monotonic.
  // A terminal Stop is history only: the Kernel keeps the newest of them in
  // its bounded resident history, and the evicted ones' journal rows are
  // pruned below with the other prior-epoch records.
  const auto stop_count = ScalarInteger(database_, "SELECT COUNT(*) FROM stop_projections");
  if (stop_count > static_cast<std::int64_t>(kMaximumRecoveredStops)) {
    Statement evict_stops(
        database_,
        "DELETE FROM stop_projections WHERE acceptance_sequence IN ("
        "SELECT p.acceptance_sequence FROM stop_projections p "
        "JOIN journal_records a ON a.sequence=p.acceptance_sequence "
        "WHERE (p.terminal_sequence IS NOT NULL OR a.owner_epoch IS NOT ("
        "SELECT owner_epoch FROM owner_epochs WHERE owner_epoch<>? "
        "ORDER BY epoch_sequence DESC LIMIT 1)) "
        "AND p.acceptance_sequence<>(SELECT acceptance_sequence FROM stop_projections "
        "ORDER BY stop_generation DESC,acceptance_sequence DESC LIMIT 1) "
        "ORDER BY p.terminal_sequence IS NULL, p.acceptance_sequence LIMIT ?)");
    evict_stops.Bind(1, owner_epoch.value());
    evict_stops.Bind(2, stop_count - static_cast<std::int64_t>(kMaximumRecoveredStops));
    Require(!evict_stops.Step(), "operation_journal_corrupt",
            "Stop truth eviction returned an unexpected row");
  }
  Statement prune_unanchored_stop_records(
      database_,
      "DELETE FROM journal_records WHERE owner_epoch<>? "
      "AND record_kind='stop_acceptance_projected' "
      "AND sequence NOT IN (SELECT acceptance_sequence FROM stop_projections) "
      "AND sequence NOT IN (SELECT snapshot_sequence FROM operation_snapshots) "
      "AND sequence IN (SELECT sequence FROM journal_records "
      "WHERE owner_epoch<>? AND record_kind='stop_acceptance_projected' "
      "AND sequence NOT IN (SELECT acceptance_sequence FROM stop_projections) "
      "AND sequence NOT IN (SELECT snapshot_sequence FROM operation_snapshots) "
      "ORDER BY sequence DESC LIMIT -1 OFFSET ?)");
  prune_unanchored_stop_records.Bind(1, owner_epoch.value());
  prune_unanchored_stop_records.Bind(2, owner_epoch.value());
  prune_unanchored_stop_records.Bind(3, static_cast<std::int64_t>(history_capacity_));
  Require(!prune_unanchored_stop_records.Step(), "operation_journal_corrupt",
          "unanchored Stop acceptance projection pruning returned an unexpected row");
  Statement prune_records(database_,
                          "DELETE FROM journal_records WHERE owner_epoch<>? AND sequence NOT IN ("
                          "SELECT snapshot_sequence FROM operation_snapshots) "
                          "AND record_kind<>'stop_acceptance_projected' "
                          "AND sequence NOT IN (SELECT terminal_sequence FROM stop_projections "
                          "WHERE terminal_sequence IS NOT NULL)");
  prune_records.Bind(1, owner_epoch.value());
  Require(!prune_records.Step(), "operation_journal_corrupt",
          "operation journal record pruning returned an unexpected row");
  Statement prune_epochs(database_,
                         "DELETE FROM owner_epochs WHERE active=0 AND owner_epoch NOT IN ("
                         "SELECT DISTINCT owner_epoch FROM journal_records) AND epoch_sequence IN ("
                         "SELECT epoch_sequence FROM owner_epochs WHERE active=0 AND owner_epoch "
                         "NOT IN (SELECT DISTINCT owner_epoch FROM journal_records) "
                         "ORDER BY epoch_sequence DESC LIMIT -1 OFFSET ?)");
  prune_epochs.Bind(1, static_cast<std::int64_t>(history_capacity_));
  Require(!prune_epochs.Step(), "operation_journal_corrupt",
          "operation owner epoch pruning returned an unexpected row");

  OperationJournalRecoveryDTO recovery;
  recovery.cell_admission_generation_high_water =
      AdmissionGeneration(static_cast<std::uint64_t>(replacement_generation));
  recovery.cell_claim_generation_high_water =
      CellAdmissionClaimGeneration(static_cast<std::uint64_t>(prior_claim_generation));
  recovery.replaced_unclosed_owner_epoch = std::move(replaced_unclosed_owner_epoch);
  Statement retained(database_,
                     "SELECT operation_id FROM operation_snapshots ORDER BY snapshot_sequence");
  while (retained.Step()) {
    recovery.historical_operations.push_back(
        LoadOperationState(database_, OperationId(retained.Text(0))));
  }
  // Each Stop payload is the single copy in its joined journal row; its
  // digest is that row's payload_digest. A NULL joined payload means "not
  // recorded" (the FKs keep every non-NULL sequence pointing at a row).
  const auto joined_payload = [](const Statement& row, const int column,
                                 const std::string_view kind) -> std::optional<std::string> {
    if (row.IsNull(column)) return std::nullopt;
    auto payload = row.Text(column);
    Require(content_identity::sha256_digest(payload) == row.Text(column + 1),
            "operation_journal_corrupt",
            "Stop " + std::string(kind) + " recovery projection digest does not match its bytes");
    return payload;
  };
  Statement retained_stops(database_, kStopProjectionRecoverySelectSql);
  while (retained_stops.Step()) {
    auto acceptance =
        DecodeStopJournalAcceptanceProjection(VerifiedAcceptedPayload(retained_stops, 0));
    Require(acceptance.ok(), "operation_journal_corrupt",
            "Stop acceptance recovery projection cannot be decoded");
    RecoveredStopJournalStateDTO recovered{std::move(acceptance.value().accepted), std::nullopt};
    if (const auto terminal_payload = joined_payload(retained_stops, 2, "terminal")) {
      auto terminal = DecodeStopJournalTerminalProjection(*terminal_payload);
      Require(terminal.ok() && terminal.value().stop_status.accepted == recovered.accepted,
              "operation_journal_corrupt",
              "Stop terminal recovery projection cannot be decoded or joined to acceptance");
      recovered.terminal_certificate = std::move(terminal.value().certificate);
    }
    recovery.recovered_stops.push_back(std::move(recovered));
  }
  Require(recovery.recovered_stops.size() <= kMaximumRecoveredStops, "operation_journal_history_full",
          "recovered Stop truth exceeds the fixed restart bound");
  Statement retained_claims(
      database_, "SELECT claim_generation FROM cell_admission_claims ORDER BY claim_generation");
  while (retained_claims.Step()) {
    auto recovered = LoadCellClaim(
        database_,
        PositiveIdentifier(retained_claims.Integer(0), "cell-admission claim generation"));
    recovered.reconcile_settlement = LoadCellReconcileSettlement(database_, recovered.claim);
    recovery.recovered_cell_admission_claims.push_back(std::move(recovered));
  }
  Require(recovery.recovered_cell_admission_claims.size() <= request_capacity,
          "operation_journal_history_full",
          "durable cell-admission identities exceed the configured owner bound");
  Statement retained_membership(
      database_,
      "SELECT owner_role,owner_epoch,request_id,semantic_intent_digest "
      "FROM cell_admission_request_membership ORDER BY owner_role,owner_epoch,request_id");
  while (retained_membership.Step()) {
    recovery.recovered_cell_admission_membership.push_back(
        {CellOwnerRole(retained_membership.Text(0)), RequestOwnerEpoch(retained_membership.Text(1)),
         RequestId(retained_membership.Text(2)), ContentDigest(retained_membership.Text(3))});
  }
  Require(recovery.recovered_cell_admission_membership.size() <= request_capacity,
          "operation_journal_history_full",
          "durable cell-admission membership exceeds the configured owner bound");
  Statement current_owners(database_,
                           "SELECT owner_role,owner_epoch FROM cell_admission_current_owners");
  while (current_owners.Step()) {
    recovery.recovered_cell_admission_current_owners.push_back(
        {CellOwnerRole(current_owners.Text(0)), RequestOwnerEpoch(current_owners.Text(1))});
  }
  Require(recovery.recovered_cell_admission_current_owners.size() <= 7U,
          "operation_journal_corrupt",
          "durable cell-admission current-owner facts exceed the closed role set");
  std::sort(recovery.recovered_cell_admission_current_owners.begin(),
            recovery.recovered_cell_admission_current_owners.end(),
            [](const auto& left, const auto& right) { return left.owner_role < right.owner_role; });
  recovery.unreconciled_resources.assign(unreconciled.begin(), unreconciled.end());
  transaction.Commit();
  return recovery;
}

std::uint64_t SqliteOperationJournal::Impl::RecordOperationAccepted(
    const RequestOwnerEpoch& owner_epoch, const OperationIntentDTO& intent,
    const OperationAcceptedDTO& accepted, const OperationStateDTO& state,
    const std::span<const OperationId> protected_operation_ids) {
  Require(intent.request_context.request_owner_epoch == owner_epoch &&
              intent.request_context.request_id == accepted.request_id &&
              accepted.replay_status == RequestReplayStatusDTO::kAcceptedNew &&
              accepted.operation_id == state.operation_id &&
              accepted.operation_revision == state.operation_revision &&
              accepted.accepted_at_ns == state.updated_at_ns &&
              accepted.participant_assignments.size() == state.participants.size() &&
              intent.participants.size() == state.participants.size(),
          "operation_journal_record_invalid",
          "operation acceptance does not match its durable intent or state");
  for (std::size_t index = 0; index < accepted.participant_assignments.size(); ++index) {
    Require(accepted.participant_assignments[index].input_index == index &&
                accepted.participant_assignments[index].participant_id ==
                    state.participants[index].participant_id &&
                intent.participants[index].execution_scope ==
                    state.participants[index].execution_scope,
            "operation_journal_record_invalid",
            "operation acceptance participant assignment is inconsistent");
  }
  return Append(owner_epoch, "operation_accepted", intent.request_context.request_id,
                RecordValue("operation_accepted", owner_epoch, OperationIntentValue(intent),
                            OperationAcceptedValue(accepted), state, nullptr, true),
                state, accepted.accepted_at_ns, true, "current", protected_operation_ids);
}

std::uint64_t SqliteOperationJournal::Impl::RecordControlAccepted(
    const RequestOwnerEpoch& owner_epoch, const OperationControlRequestDTO& request,
    const OperationControlAcceptedDTO& accepted, const OperationStateDTO& state) {
  Require(request.request_context.request_owner_epoch == owner_epoch &&
              request.request_context.request_id == accepted.request_id &&
              request.operation_id == accepted.operation_id &&
              request.operation_id == state.operation_id &&
              accepted.operation_revision_after_acceptance == state.operation_revision &&
              accepted.replay_status == RequestReplayStatusDTO::kAcceptedNew &&
              accepted.accepted_at_ns == state.updated_at_ns,
          "operation_journal_record_invalid",
          "operation control acceptance does not match its durable request or state");
  return Append(owner_epoch, "control_accepted", request.request_context.request_id,
                RecordValue("control_accepted", owner_epoch, ControlRequestValue(request),
                            ControlAcceptedValue(accepted), state, nullptr, true),
                state, accepted.accepted_at_ns, true, "current");
}

std::uint64_t SqliteOperationJournal::Impl::RecordStopAccepted(
    const RequestOwnerEpoch& owner_epoch, const EmergencyStopAcceptanceRecordDTO& acceptance,
    const EmergencyStopProjectedIntentDTO& intent, const StopAcceptedDTO& accepted,
    const EmergencyStopAffectedStateSnapshotDTO& affected_state) {
  RequireEmergencyStopProjection(owner_epoch, acceptance, intent, accepted, affected_state);
  return AppendStopAcceptance(
      owner_epoch, acceptance,
      EmergencyStopAcceptanceProjectionValue(acceptance, intent, accepted, affected_state),
      accepted, affected_state);
}

std::uint64_t SqliteOperationJournal::Impl::RecordStopTerminal(
    const RequestOwnerEpoch& owner_epoch, const StopStatusDTO& stop_status,
    const StopCertificateDTO& certificate) {
  Require(!owner_epoch.empty(), "operation_journal_record_invalid",
          "Stop terminal projection owner epoch is empty");
  Require(!stop_terminal_projection_field_error(stop_status, certificate).has_value(),
          "operation_journal_record_invalid", "Stop terminal projection is invalid");
  auto recorded_status = stop_status;
  recorded_status.terminal_projection = StopDurableProjectionStatusDTO{
      StopDurableProjectionStateDTO::kRecorded, std::nullopt, std::nullopt};
  StopJournalTerminalProjectionDTO projection{std::move(recorded_status), certificate};
  auto encoded = EncodeStopJournalTerminalProjection(projection);
  Require(encoded.ok(), "operation_journal_record_invalid", "Stop terminal projection is invalid");
  return AppendStopTerminal(owner_epoch, projection, encoded.value());
}

std::uint64_t SqliteOperationJournal::Impl::CloseOwnerEpoch(const RequestOwnerEpoch& owner_epoch,
                                                            const std::int64_t closed_at_ns) {
  Require(!owner_epoch.empty() && closed_at_ns >= 0, "operation_journal_owner_epoch_invalid",
          "owner close identity is invalid");
  const auto lock = LockOrdinaryWriter();
  RequireOpen();
  const auto occurred_at_wall = wall_time_text_();
  ValidateWallTimeText(occurred_at_wall);
  Transaction transaction(database_);
  Statement owner(database_,
                  "SELECT active,close_sequence,closed_at_ns FROM owner_epochs "
                  "WHERE owner_epoch=?");
  owner.Bind(1, owner_epoch.value());
  Require(owner.Step(), "operation_journal_owner_epoch_stale",
          "owner close names a non-active owner epoch");
  if (!owner.IsNull(1)) {
    const auto sequence = PositiveIdentifier(owner.Integer(1), "operation journal sequence");
    Require(owner.Integer(2) == closed_at_ns && !owner.Step(),
            "operation_journal_owner_close_conflict",
            "owner close replay uses a different close time");
    transaction.Commit();
    return sequence;
  }
  Require(owner.Integer(0) == 1 && !owner.Step(), "operation_journal_owner_epoch_stale",
          "owner close names a non-active owner epoch");
  Statement operation_blockers(
      database_,
      "SELECT COUNT(*) FROM operation_snapshots WHERE disposition='orphaned_by_host_replacement' "
      "OR state NOT IN ('completed','stopped','failed','intervention_required',"
      "'cancelled_pre_dispatch')");
  Require(
      operation_blockers.Step() && operation_blockers.Integer(0) == 0 && !operation_blockers.Step(),
      "operation_journal_owner_close_blocked", "owner close is blocked by unresolved durable work");
  Statement stop_blockers(database_, kStopProjectionCloseBlockersSelectSql);
  while (stop_blockers.Step()) {
    Require(!stop_blockers.IsNull(0), "operation_journal_owner_close_blocked",
            "owner close is blocked by a nonterminal Stop");
    auto terminal = DecodeStopJournalTerminalProjection(stop_blockers.Text(0));
    Require(terminal.ok(), "operation_journal_corrupt",
            "owner close cannot decode terminal Stop truth");
  }
  Statement claim_blockers(
      database_,
      "SELECT COUNT(*) FROM cell_admission_claims WHERE release_owner_role IS NULL "
      "AND reconciled_sequence IS NULL "
      "AND claim_kind NOT IN ('authoring_mutation','manager_apply')");
  Require(claim_blockers.Step() && claim_blockers.Integer(0) == 0 && !claim_blockers.Step(),
          "operation_journal_owner_close_blocked",
          "owner close is blocked by unresolved execution/effect claims");
  const auto payload = content_identity::canonical_json(CanonicalValue(CanonicalValue::Object{
      {"closed_at_ns", Timestamp(closed_at_ns, "owner close timestamp")},
      {"owner_epoch", CanonicalValue(owner_epoch.value())},
  }));
  Statement insert(database_,
                   "INSERT INTO journal_records(owner_epoch,record_kind,request_id,operation_id,"
                   "operation_revision,occurred_at_ns,occurred_at_wall,effect_pending,payload_json,"
                   "payload_digest) VALUES(?,'owner_epoch_closed',NULL,NULL,NULL,?,?,0,?,?)");
  insert.Bind(1, owner_epoch.value());
  insert.Bind(2, closed_at_ns);
  insert.Bind(3, occurred_at_wall);
  insert.Bind(4, payload);
  insert.Bind(5, content_identity::sha256_digest(payload));
  Require(!insert.Step(), "operation_journal_corrupt",
          "owner close journal insert returned an unexpected row");
  const auto sequence =
      PositiveIdentifier(sqlite3_last_insert_rowid(database_), "operation journal sequence");
  Statement update(database_,
                   "UPDATE owner_epochs SET closed_at_ns=?,close_sequence=?,active=0 "
                   "WHERE owner_epoch=? AND active=1 AND close_sequence IS NULL");
  update.Bind(1, closed_at_ns);
  update.Bind(2, static_cast<std::int64_t>(sequence));
  update.Bind(3, owner_epoch.value());
  Require(!update.Step() && sqlite3_changes(database_) == 1, "operation_journal_corrupt",
          "owner close update did not affect exactly one row");
  transaction.Commit();
  return sequence;
}

std::uint64_t SqliteOperationJournal::Impl::RecordRuntimeScaleAccepted(
    const RequestOwnerEpoch& owner_epoch, const RuntimeScaleRequestDTO& request,
    const RuntimeScaleAcceptedDTO& accepted, const OperationStateDTO& state,
    const RuntimeScaleStatusDTO& scale_status) {
  Require(request.request_context.request_owner_epoch == owner_epoch &&
              request.request_context.request_id == accepted.request_id &&
              request.operation_id == accepted.operation_id &&
              request.operation_id == state.operation_id &&
              accepted.operation_revision == state.operation_revision &&
              request.scale == accepted.requested_scale &&
              accepted.runtime_scale_generation == scale_status.runtime_scale_generation &&
              state.runtime_scale_generation == accepted.runtime_scale_generation &&
              scale_status.operation_id == state.operation_id &&
              scale_status.operation_revision == state.operation_revision &&
              accepted.replay_status == RequestReplayStatusDTO::kAcceptedNew &&
              accepted.accepted_at_ns == state.updated_at_ns &&
              scale_status.updated_at_ns == state.updated_at_ns,
          "operation_journal_record_invalid",
          "runtime-scale acceptance does not match its durable request or state");
  return Append(
      owner_epoch, "runtime_scale_accepted", request.request_context.request_id,
      RecordValue("runtime_scale_accepted", owner_epoch, RuntimeScaleRequestValue(request),
                  RuntimeScaleAcceptedValue(accepted), state, &scale_status, true),
      state, accepted.accepted_at_ns, true, "current");
}

std::uint64_t SqliteOperationJournal::Impl::RecordOperationState(
    const RequestOwnerEpoch& owner_epoch, const OperationStateDTO& state,
    const RuntimeScaleStatusDTO* scale_status, const char* transition_reason,
    const std::span<const PreparedMotionDiagnosticsDTO> prepared_motion_diagnostics) {
  const std::string_view reason = transition_reason == nullptr ? "" : transition_reason;
  const auto motion_participant_count = static_cast<std::size_t>(std::count_if(
      state.participants.begin(), state.participants.end(), [](const auto& participant) {
        return participant.payload_kind == OperationParticipantPayloadKind::kMotion;
      }));
  const bool exact_motion_prepared = reason == "subordinate_motion_prepared";
  const bool coordinated_prepared = reason == "coordinated_prepare_barrier_complete";
  const bool prepared_diagnostics_expected =
      exact_motion_prepared || (coordinated_prepared && motion_participant_count != 0U);
  Require(!reason.empty() &&
              (scale_status == nullptr ||
               (scale_status->operation_id == state.operation_id &&
                scale_status->operation_revision == state.operation_revision &&
                state.runtime_scale_generation == scale_status->runtime_scale_generation)) &&
              prepared_diagnostics_expected == !prepared_motion_diagnostics.empty() &&
              (!prepared_diagnostics_expected ||
               prepared_motion_diagnostics.size() == motion_participant_count),
          "operation_journal_record_invalid",
          "operation state observation is missing exact transition evidence");
  CanonicalValue::Object intent{
      {"transition_reason", CanonicalValue(std::string(reason))},
  };
  if (!prepared_motion_diagnostics.empty()) {
    intent.emplace_back("prepared_motion_diagnostics",
                        PreparedMotionDiagnosticsValue(state, prepared_motion_diagnostics));
  }
  const std::string_view record_kind =
      prepared_diagnostics_expected ? "operation_prepared" : "state_observed";
  return Append(owner_epoch, record_kind, std::nullopt,
                RecordValue(record_kind, owner_epoch, CanonicalValue(std::move(intent)),
                            CanonicalValue(nullptr), state, scale_status, false),
                state, state.updated_at_ns, false, "current");
}

std::uint64_t SqliteOperationJournal::Impl::RecordCellAdmissionClaimAcquired(
    const RequestOwnerEpoch& owner_epoch, const CellAdmissionClaimDTO& claim,
    const CellAdmissionCurrentOwnerEpochDTO* verified_manager,
    const std::vector<CellAdmissionClaimDTO>* expected_held_claims,
    const std::vector<CellAdmissionCurrentOwnerEpochDTO>* expected_current_owners,
    CellAdmissionClaimDTO* persisted_claim) {
  const bool configuration_apply = verified_manager != nullptr;
  Require(ValidateCellAdmissionClaimIntent(claim.intent).ok() && claim.claim_generation.nonzero() &&
              claim.acquired_admission_generation.nonzero() && claim.accepted_at_ns >= 0,
          "operation_journal_record_invalid", "cell-admission acquire record is invalid");
  const auto lock = [&] {
    AdmissionSqlTiming timing("acquire_writer_wait");
    return LockOrdinaryWriter();
  }();
  if (critical_section_observer_) {
    critical_section_observer_("cell_admission_acquire");
  }
  RequireOpen();
  AdmissionSqlTiming transaction_timing("acquire_transaction_scope");
  Transaction transaction(database_, true, false);
  Statement active(database_,
                   "SELECT request_capacity FROM owner_epochs "
                   "WHERE owner_epoch=? AND active=1", "acquire_active_step");
  active.Bind(1, owner_epoch.value());
  Require(active.Step(), "operation_journal_owner_epoch_stale",
          "cell-admission acquire names a non-active Kernel owner epoch");
  const auto capacity = active.Integer(0);
  Require(capacity > 0 && !active.Step(), "operation_journal_corrupt",
          "active owner capacity is invalid");
  auto semantic_digest = CellAdmissionClaimIntentDigest(claim.intent);
  Require(semantic_digest.ok(), "operation_journal_record_invalid",
          "cell-admission acquire intent digest is invalid");
  Require(
      !configuration_apply || (expected_held_claims && expected_current_owners && persisted_claim),
      "operation_journal_record_invalid", "configuration Apply proof inputs are missing");
  auto resulting_owners = configuration_apply ? *expected_current_owners
                                              : std::vector<CellAdmissionCurrentOwnerEpochDTO>{};
  if (configuration_apply) {
    Require(expected_held_claims && expected_current_owners && persisted_claim &&
                ConfigurationApplyClaim(claim) &&
                verified_manager->owner_role == CellAdmissionOwnerRoleDTO::kManager &&
                verified_manager->owner_epoch == claim.intent.owner_epoch &&
                std::is_sorted(expected_held_claims->begin(), expected_held_claims->end(),
                               [](const auto& a, const auto& b) {
                                 return a.claim_generation < b.claim_generation;
                               }) &&
                std::all_of(expected_held_claims->begin(), expected_held_claims->end(),
                            ConfigurationApplyClaim),
            "operation_journal_record_invalid", "configuration Apply proof shape is invalid");
    const auto prior = std::find_if(
        resulting_owners.begin(), resulting_owners.end(),
        [](const auto& row) { return row.owner_role == CellAdmissionOwnerRoleDTO::kManager; });
    if (prior == resulting_owners.end())
      resulting_owners.push_back(*verified_manager);
    else
      *prior = *verified_manager;
    std::sort(resulting_owners.begin(), resulting_owners.end(),
              [](const auto& a, const auto& b) { return a.owner_role < b.owner_role; });
  }
  Statement current_owner(
      database_, "SELECT owner_epoch FROM cell_admission_current_owners WHERE owner_role=?", "acquire_current_owner_step");
  current_owner.Bind(1, to_string(claim.intent.owner_role));
  if (current_owner.Step()) {
    const bool matches = current_owner.Text(0) == claim.intent.owner_epoch.value();
    Require((configuration_apply || matches) && !current_owner.Step(), "stale_request_owner_epoch",
            "cell-admission request names a stale request-owner epoch");
  }
  Statement membership(
      database_,
      "SELECT semantic_intent_digest,claim_generation "
      "FROM cell_admission_request_membership WHERE owner_epoch=? AND request_id=?", "acquire_membership_step");
  membership.Bind(1, claim.intent.owner_epoch.value());
  membership.Bind(2, claim.intent.request_id.value());
  if (membership.Step()) {
    const auto same = membership.Text(0) == semantic_digest.value().value();
    const auto existing_generation =
        PositiveIdentifier(membership.Integer(1), "cell-admission claim generation");
    Require(!membership.Step(), "operation_journal_corrupt",
            "cell-admission membership identity is duplicated");
    Require(same, "request_id_conflict",
            "cell-admission request reuses compact membership with different intent");
    Statement resident(database_,
                       "SELECT COUNT(*) FROM cell_admission_claims WHERE claim_generation=? "
                       "AND release_owner_role IS NULL AND reconciled_sequence IS NULL", "acquire_resident_step");
    resident.Bind(1, static_cast<std::int64_t>(existing_generation));
    Require(resident.Step(), "operation_journal_corrupt",
            "cell-admission resident membership query returned no row");
    const bool has_result = resident.Integer(0) == 1;
    Require(!resident.Step(), "operation_journal_corrupt",
            "cell-admission resident membership query returned multiple rows");
    Require(has_result, "request_result_expired",
            "cell-admission exact request result was compacted");
  }
  Statement replay(database_,
                   "SELECT claim_generation FROM cell_admission_claims "
                   "WHERE owner_epoch=? AND request_id=? "
                   "AND release_owner_role IS NULL AND reconciled_sequence IS NULL", "acquire_replay_step");
  replay.Bind(1, claim.intent.owner_epoch.value());
  replay.Bind(2, claim.intent.request_id.value());
  if (replay.Step()) {
    const auto existing_generation =
        PositiveIdentifier(replay.Integer(0), "cell-admission claim generation");
    Require(!replay.Step(), "operation_journal_corrupt",
            "cell-admission request identity is duplicated");
    const auto existing = LoadCellClaim(database_, existing_generation, true);
    if (configuration_apply) {
      auto expected_replay = *expected_held_claims;
      expected_replay.push_back(existing.claim);
      Require(
          existing.claim.intent == claim.intent &&
              existing.claim.claim_generation == claim.claim_generation &&
              existing.claim.acquired_admission_generation == claim.acquired_admission_generation &&
              LoadHeldCellClaims(database_, true) == expected_replay &&
              LoadCellCurrentOwners(database_) == resulting_owners &&
              ScalarInteger(database_,
                            "SELECT cell_admission_generation_high_water "
                            "FROM journal_metadata WHERE singleton=1") ==
                  static_cast<std::int64_t>(claim.acquired_admission_generation.value() + 1U),
          "operation_journal_request_conflict",
          "configuration Apply replay lacks exact durable inventory and membership");
      *persisted_claim = existing.claim;
    } else {
      Require(existing.claim == claim, "operation_journal_request_conflict",
              "cell-admission acquire replay conflicts with durable truth");
    }
    transaction.Commit();
    return existing_generation;
  }
  if (configuration_apply) {
    Require(LoadHeldCellClaims(database_, true) == *expected_held_claims &&
                LoadCellCurrentOwners(database_) == *expected_current_owners,
            "operation_journal_request_conflict",
            "configuration Apply inventory or current-owner membership changed");
    Statement adopt(database_,
                    "INSERT INTO cell_admission_current_owners(owner_role,owner_epoch,"
                    "established_kernel_owner_epoch) VALUES(?,?,?) ON CONFLICT(owner_role) "
                    "DO UPDATE SET owner_epoch=excluded.owner_epoch,"
                    "established_kernel_owner_epoch=excluded.established_kernel_owner_epoch", "acquire_adopt_step");
    adopt.Bind(1, to_string(verified_manager->owner_role));
    adopt.Bind(2, verified_manager->owner_epoch.value());
    adopt.Bind(3, owner_epoch.value());
    Require(!adopt.Step(), "operation_journal_corrupt",
            "Manager membership adoption returned a row");
  }
  Statement retire_membership(
      database_,
      "DELETE FROM cell_admission_request_membership WHERE claim_generation IN ("
      "SELECT claim_generation FROM cell_admission_claims "
      "WHERE owner_role=? AND owner_epoch=? "
      "AND release_owner_role IS NOT NULL AND reconciled_sequence IS NULL)", "acquire_retire_membership_step");
  retire_membership.Bind(1, to_string(claim.intent.owner_role));
  retire_membership.Bind(2, claim.intent.owner_epoch.value());
  Require(!retire_membership.Step(), "operation_journal_corrupt",
          "cell-admission released membership retirement returned a row");
  Statement retire_claim(database_,
                         "DELETE FROM cell_admission_claims "
                         "WHERE owner_role=? AND owner_epoch=? "
                         "AND release_owner_role IS NOT NULL AND reconciled_sequence IS NULL", "acquire_retire_claim_step");
  retire_claim.Bind(1, to_string(claim.intent.owner_role));
  retire_claim.Bind(2, claim.intent.owner_epoch.value());
  Require(!retire_claim.Step(), "operation_journal_corrupt",
          "cell-admission released result retirement returned a row");
  Statement count(database_,
                  "SELECT COUNT(*),COALESCE(MAX(claim_generation),0) "
                  "FROM cell_admission_request_membership", "acquire_count_step");
  Require(count.Step(), "operation_journal_corrupt",
          "cell-admission capacity query returned no row");
  const auto resident = count.Integer(0);
  Require(!count.Step() && resident >= 0 && resident < capacity, "request_ledger_full",
          "cell-admission durable membership is exhausted");
  Statement high_water(database_,
                       "SELECT cell_admission_generation_high_water,"
                       "cell_claim_generation_high_water FROM journal_metadata WHERE singleton=1", "acquire_high_water_step");
  Require(high_water.Step(), "operation_journal_corrupt",
          "cell-admission high-water query returned no row");
  const auto admission_high_water = high_water.Integer(0);
  const auto claim_high_water = high_water.Integer(1);
  Require(!high_water.Step() && admission_high_water > 0 && claim_high_water >= 0,
          "operation_journal_corrupt", "cell-admission high-water is invalid");
  Require(static_cast<std::uint64_t>(admission_high_water) ==
                  claim.acquired_admission_generation.value() &&
              static_cast<std::uint64_t>(claim_high_water) + 1U == claim.claim_generation.value(),
          "operation_journal_record_invalid",
          "cell-admission claim is not the durable generation successor");

  Statement bind_owner(database_,
                       "INSERT OR IGNORE INTO cell_admission_current_owners("
                       "owner_role,owner_epoch,established_kernel_owner_epoch) VALUES(?,?,?)", "acquire_bind_owner_step");
  bind_owner.Bind(1, to_string(claim.intent.owner_role));
  bind_owner.Bind(2, claim.intent.owner_epoch.value());
  bind_owner.Bind(3, owner_epoch.value());
  Require(!bind_owner.Step(), "operation_journal_corrupt",
          "cell-admission current-owner binding returned an unexpected row");
  Statement insert_membership(
      database_,
      "INSERT INTO cell_admission_request_membership("
      "owner_role,owner_epoch,request_id,semantic_intent_digest,claim_generation) "
      "VALUES(?,?,?,?,?)", "acquire_insert_membership_step");
  insert_membership.Bind(1, to_string(claim.intent.owner_role));
  insert_membership.Bind(2, claim.intent.owner_epoch.value());
  insert_membership.Bind(3, claim.intent.request_id.value());
  insert_membership.Bind(4, semantic_digest.value().value());
  insert_membership.Bind(5, static_cast<std::int64_t>(claim.claim_generation.value()));
  Require(!insert_membership.Step(), "operation_journal_corrupt",
          "cell-admission membership insert returned an unexpected row");

  Statement insert(
      database_,
      "INSERT INTO cell_admission_claims("
      "claim_generation,kernel_owner_epoch,claim_kind,owner_role,owner_epoch,"
      "owner_transaction_id,request_id,request_payload_digest,expected_admission_generation,"
      "acquired_admission_generation,accepted_at_ns,draft_program_revision,"
      "applied_program_revision,offering_owner_role,offering_owner_epoch,offer_transaction_id,"
      "offer_digest,parent_program_cycle_claim_generation,program_node_runner_owner_epoch,"
      "program_node_run_id,program_node_instance_id,program_node_attempt_id,program_node_owner_api)"
      " "
      "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)", "acquire_insert_step");
  insert.Bind(1, static_cast<std::int64_t>(claim.claim_generation.value()));
  insert.Bind(2, owner_epoch.value());
  insert.Bind(3, to_string(claim.intent.claim_kind));
  insert.Bind(4, to_string(claim.intent.owner_role));
  insert.Bind(5, claim.intent.owner_epoch.value());
  insert.Bind(6, claim.intent.owner_transaction_id);
  insert.Bind(7, claim.intent.request_id.value());
  insert.Bind(8, claim.intent.request_payload_digest.value());
  insert.Bind(9, static_cast<std::int64_t>(claim.intent.expected_admission_generation.value()));
  insert.Bind(10, static_cast<std::int64_t>(claim.acquired_admission_generation.value()));
  insert.Bind(11, claim.accepted_at_ns);
  if (claim.intent.program_cycle_evidence.has_value()) {
    insert.Bind(12, claim.intent.program_cycle_evidence->draft_program_revision.value());
    insert.Bind(13, claim.intent.program_cycle_evidence->applied_program_revision.value());
  } else {
    insert.BindNull(12);
    insert.BindNull(13);
  }
  if (claim.intent.intervention_evidence.has_value()) {
    const auto& evidence = *claim.intent.intervention_evidence;
    insert.Bind(14, to_string(evidence.offering_owner_role));
    insert.Bind(15, evidence.offering_owner_epoch.value());
    insert.Bind(16, evidence.offer_transaction_id);
    insert.Bind(17, evidence.offer_digest.value());
    if (evidence.parent_program_cycle_claim_generation.has_value()) {
      insert.Bind(
          18, static_cast<std::int64_t>(evidence.parent_program_cycle_claim_generation->value()));
    } else {
      insert.BindNull(18);
    }
  } else {
    for (int index = 14; index <= 18; ++index) insert.BindNull(index);
  }
  if (claim.intent.program_node_evidence.has_value()) {
    const auto& evidence = *claim.intent.program_node_evidence;
    insert.Bind(18,
                static_cast<std::int64_t>(evidence.parent_program_cycle_claim_generation.value()));
    insert.Bind(19, evidence.program_runner_owner_epoch.value());
    insert.Bind(20, evidence.program_run_id.value());
    insert.Bind(21, evidence.node_instance_id);
    insert.Bind(22, evidence.node_attempt_id);
    insert.Bind(23, evidence.owner_api);
  } else {
    for (int index = 19; index <= 23; ++index) insert.BindNull(index);
  }
  Require(!insert.Step(), "operation_journal_corrupt",
          "cell-admission claim insert returned an unexpected row");
  for (std::size_t index = 0; index < claim.intent.scope.resources.size(); ++index) {
    Statement resource(
        database_,
        "INSERT INTO cell_admission_claim_resources(claim_generation,resource_index,resource_id)"
        " VALUES(?,?,?)", "acquire_resource_step");
    resource.Bind(1, static_cast<std::int64_t>(claim.claim_generation.value()));
    resource.Bind(2, static_cast<std::int64_t>(index));
    resource.Bind(3, claim.intent.scope.resources[index].value());
    Require(!resource.Step(), "operation_journal_corrupt",
            "cell-admission resource insert returned an unexpected row");
  }
  for (std::size_t index = 0; index < claim.intent.scope.configurations.size(); ++index) {
    const auto& configuration = claim.intent.scope.configurations[index];
    Statement row(database_,
                  "INSERT INTO cell_admission_claim_configurations("
                  "claim_generation,configuration_index,control_domain_id,"
                  "active_configuration_id,active_configuration_generation) VALUES(?,?,?,?,?)", "acquire_row_step");
    row.Bind(1, static_cast<std::int64_t>(claim.claim_generation.value()));
    row.Bind(2, static_cast<std::int64_t>(index));
    row.Bind(3, configuration.control_domain_id.value());
    row.Bind(4, configuration.active_configuration_id.value());
    row.Bind(5, static_cast<std::int64_t>(configuration.active_configuration_generation.value()));
    Require(!row.Step(), "operation_journal_corrupt",
            "cell-admission configuration insert returned an unexpected row");
  }
  Statement advance(
      database_,
      "UPDATE journal_metadata SET cell_admission_generation_high_water=?,"
      "cell_claim_generation_high_water=? WHERE singleton=1 "
      "AND cell_admission_generation_high_water=? AND cell_claim_generation_high_water=?", "acquire_advance_step");
  advance.Bind(1, static_cast<std::int64_t>(claim.acquired_admission_generation.value() + 1U));
  advance.Bind(2, static_cast<std::int64_t>(claim.claim_generation.value()));
  advance.Bind(3, static_cast<std::int64_t>(claim.acquired_admission_generation.value()));
  advance.Bind(4, claim_high_water);
  Require(!advance.Step() && sqlite3_changes(database_) == 1, "operation_journal_record_invalid",
          "cell-admission acquire generation did not advance");
  if (persisted_claim) *persisted_claim = claim;
  if (configuration_apply) {
    // Prepare the expected postimage before COMMIT; normal success returns by move only.
    auto expected_committed = *expected_held_claims;
    expected_committed.push_back(claim);
    CommitConfigurationApply(transaction, database_, claim, [&] {
      const auto held = LoadHeldCellClaims(database_, true);
      const auto owners = LoadCellCurrentOwners(database_);
      const auto generation = ScalarInteger(
          database_,
          "SELECT cell_admission_generation_high_water FROM journal_metadata WHERE singleton=1");
      if (held == expected_committed && owners == resulting_owners &&
          generation ==
              static_cast<std::int64_t>(claim.acquired_admission_generation.value() + 1U)) {
        return ConfigurationCommitReadback::kCommitted;
      }
      if (held == *expected_held_claims && owners == *expected_current_owners &&
          generation == static_cast<std::int64_t>(claim.acquired_admission_generation.value())) {
        return ConfigurationCommitReadback::kAbsent;
      }
      return ConfigurationCommitReadback::kUnknown;
    });
  } else {
    transaction.Commit();
  }
  return claim.claim_generation.value();
}

std::uint64_t SqliteOperationJournal::Impl::RecordCellAdmissionClaimReleased(
    const RequestOwnerEpoch& owner_epoch, const CellAdmissionClaimDTO& claim,
    const CellAdmissionClaimReleaseDTO& release, const AdmissionGeneration resulting_generation) {
  Require(ValidateCellAdmissionClaimRelease(release).ok() &&
              claim.claim_generation == release.claim_generation &&
              claim.intent.owner_role == release.owner_role &&
              claim.intent.owner_epoch == release.owner_epoch &&
              claim.intent.owner_transaction_id == release.owner_transaction_id &&
              claim.intent.request_id == release.request_id &&
              claim.intent.request_payload_digest == release.request_payload_digest &&
              claim.acquired_admission_generation == release.acquired_admission_generation &&
              resulting_generation.nonzero() && resulting_generation.value() <= kMaximumSafeInteger,
          "operation_journal_record_invalid", "cell-admission release record is invalid");
  const auto lock = [&] {
    AdmissionSqlTiming timing("release_writer_wait");
    return LockOrdinaryWriter();
  }();
  RequireOpen();
  AdmissionSqlTiming transaction_timing("release_transaction_scope");
  Transaction transaction(database_, true, true);
  Statement active(database_, "SELECT COUNT(*) FROM owner_epochs WHERE owner_epoch=? AND active=1", "release_active_step");
  active.Bind(1, owner_epoch.value());
  Require(active.Step() && active.Integer(0) == 1 && !active.Step(),
          "operation_journal_owner_epoch_stale",
          "cell-admission release names a non-active Kernel owner epoch");
  Statement current_owner(
      database_, "SELECT owner_epoch FROM cell_admission_current_owners WHERE owner_role=?", "release_current_owner_step");
  current_owner.Bind(1, to_string(claim.intent.owner_role));
  if (current_owner.Step()) {
    Require(current_owner.Text(0) == claim.intent.owner_epoch.value() && !current_owner.Step(),
            "operation_journal_owner_epoch_stale",
            "cell-admission release names a stale request-owner epoch");
  }
  Statement resident(database_,
                     "SELECT COUNT(*) FROM cell_admission_claims WHERE claim_generation=?", "release_resident_step");
  resident.Bind(1, static_cast<std::int64_t>(claim.claim_generation.value()));
  Require(resident.Step(), "operation_journal_corrupt",
          "cell-admission release resident query returned no row");
  const bool has_resident_claim = resident.Integer(0) == 1;
  Require(!resident.Step(), "operation_journal_corrupt",
          "cell-admission release resident query returned multiple rows");
  if (!has_resident_claim) {
    Statement compacted(database_,
                        "SELECT semantic_intent_digest FROM cell_admission_request_membership "
                        "WHERE owner_epoch=? AND request_id=?", "release_compacted_step");
    compacted.Bind(1, claim.intent.owner_epoch.value());
    compacted.Bind(2, claim.intent.request_id.value());
    auto digest = CellAdmissionClaimIntentDigest(claim.intent);
    Require(compacted.Step() && digest.ok() && compacted.Text(0) == digest.value().value() &&
                !compacted.Step(),
            "operation_journal_request_conflict",
            "cell-admission release conflicts with durable membership");
    Fail("operation_journal_owner_epoch_stale",
         "cell-admission predecessor claim is already terminal or reconciled");
  }
  auto existing = LoadCellClaim(database_, claim.claim_generation.value(), true, true);
  Require(existing.claim == claim, "operation_journal_request_conflict",
          "cell-admission release conflicts with the durable claim");
  Statement reconciled(database_,
                       "SELECT reconciled_sequence FROM cell_admission_claims "
                       "WHERE claim_generation=?", "release_reconciled_step");
  reconciled.Bind(1, static_cast<std::int64_t>(claim.claim_generation.value()));
  Require(reconciled.Step() && reconciled.IsNull(0) && !reconciled.Step(),
          "operation_journal_owner_epoch_stale",
          "cell-admission predecessor claim was settled by reconciliation");
  if (existing.release.has_value()) {
    Require(*existing.release == release, "operation_journal_request_conflict",
            "cell-admission release replay conflicts with durable truth");
    transaction.Commit();
    return claim.claim_generation.value();
  }
  Statement update(
      database_,
      "UPDATE cell_admission_claims SET release_owner_role=?,release_owner_epoch=?,"
      "release_owner_transaction_id=?,release_request_id=?,release_request_payload_digest=?,"
      "release_acquired_admission_generation=?,release_settlement_kind=?,"
      "release_evidence_digest=? WHERE claim_generation=? AND release_owner_role IS NULL", "release_update_step");
  update.Bind(1, to_string(release.owner_role));
  update.Bind(2, release.owner_epoch.value());
  update.Bind(3, release.owner_transaction_id);
  update.Bind(4, release.request_id.value());
  update.Bind(5, release.request_payload_digest.value());
  update.Bind(6, static_cast<std::int64_t>(release.acquired_admission_generation.value()));
  update.Bind(7, to_string(release.settlement.settlement_kind));
  update.Bind(8, release.settlement.evidence_digest.value());
  update.Bind(9, static_cast<std::int64_t>(release.claim_generation.value()));
  Require(!update.Step() && sqlite3_changes(database_) == 1, "operation_journal_corrupt",
          "cell-admission release did not update exactly one claim");
  Statement advance(
      database_,
      "UPDATE journal_metadata SET cell_admission_generation_high_water=? WHERE singleton=1 "
      "AND cell_admission_generation_high_water=?", "release_advance_step");
  advance.Bind(1, static_cast<std::int64_t>(resulting_generation.value()));
  advance.Bind(2, static_cast<std::int64_t>(resulting_generation.value() - 1U));
  Require(!advance.Step() && sqlite3_changes(database_) == 1, "operation_journal_record_invalid",
          "cell-admission release generation did not advance");
  transaction.Commit();
  return claim.claim_generation.value();
}

AdmissionGeneration SqliteOperationJournal::Impl::RecordTerminalApplyClaimsReleasedBySuccess(
    const RequestOwnerEpoch& owner_epoch, const CellAdmissionClaimDTO& fresh_claim,
    const CellAdmissionClaimReleaseDTO& fresh_success_release,
    const std::vector<CellAdmissionClaimDTO>& expected_held_claims,
    const std::vector<CellAdmissionCurrentOwnerEpochDTO>& expected_current_owners,
    const AdmissionGeneration expected_admission_generation) {
  Require(ConfigurationApplyClaim(fresh_claim) &&
              ValidateCellAdmissionClaimRelease(fresh_success_release).ok() &&
              fresh_success_release ==
                  CellAdmissionClaimReleaseDTO{
                      fresh_claim.intent.owner_role, fresh_claim.intent.owner_epoch,
                      fresh_claim.intent.owner_transaction_id, fresh_claim.intent.request_id,
                      fresh_claim.intent.request_payload_digest,
                      fresh_claim.acquired_admission_generation, fresh_claim.claim_generation,
                      fresh_success_release.settlement} &&
              fresh_success_release.settlement.settlement_kind ==
                  CellAdmissionSettlementKindDTO::kTerminalEffectsSettled &&
              expected_admission_generation.nonzero() &&
              expected_admission_generation.value() < kMaximumSafeInteger &&
              std::is_sorted(expected_held_claims.begin(), expected_held_claims.end(),
                             [](const auto& a, const auto& b) {
                               return a.claim_generation < b.claim_generation;
                             }) &&
              std::adjacent_find(expected_held_claims.begin(), expected_held_claims.end(),
                                 [](const auto& a, const auto& b) {
                                   return a.claim_generation == b.claim_generation;
                                 }) == expected_held_claims.end() &&
              std::all_of(expected_held_claims.begin(), expected_held_claims.end(),
                          ConfigurationApplyClaim) &&
              std::find(expected_held_claims.begin(), expected_held_claims.end(), fresh_claim) !=
                  expected_held_claims.end(),
          "operation_journal_record_invalid", "fresh Apply success release is not exact");
  const auto lock = LockOrdinaryWriter();
  RequireOpen();
  Transaction transaction(database_);
  Statement active(database_, "SELECT COUNT(*) FROM owner_epochs WHERE owner_epoch=? AND active=1");
  active.Bind(1, owner_epoch.value());
  Require(active.Step() && active.Integer(0) == 1 && !active.Step(),
          "operation_journal_owner_epoch_stale", "fresh Apply names a non-active Kernel owner");
  Require(LoadCellCurrentOwners(database_) == expected_current_owners &&
              std::find(expected_current_owners.begin(), expected_current_owners.end(),
                        CellAdmissionCurrentOwnerEpochDTO{CellAdmissionOwnerRoleDTO::kManager,
                                                          fresh_claim.intent.owner_epoch}) !=
                  expected_current_owners.end(),
          "operation_journal_request_conflict", "fresh Apply current-owner membership changed");
  const auto durable_held = LoadHeldCellClaims(database_);
  const auto generation = ScalarInteger(
      database_,
      "SELECT cell_admission_generation_high_water FROM journal_metadata WHERE singleton=1");
  const bool has_old = expected_held_claims.size() > 1U;
  const AdmissionGeneration resulting(expected_admission_generation.value() + (has_old ? 1U : 0U));
  // Replay is exact readback after a possibly unknown commit, not a second release.
  if (has_old && durable_held == std::vector<CellAdmissionClaimDTO>{fresh_claim}) {
    for (const auto& old : expected_held_claims) {
      if (old == fresh_claim) continue;
      const auto row = LoadCellClaim(database_, old.claim_generation.value());
      const CellAdmissionClaimReleaseDTO release{old.intent.owner_role,
                                                 old.intent.owner_epoch,
                                                 old.intent.owner_transaction_id,
                                                 old.intent.request_id,
                                                 old.intent.request_payload_digest,
                                                 old.acquired_admission_generation,
                                                 old.claim_generation,
                                                 fresh_success_release.settlement};
      Require(row.claim == old && row.release == release &&
                  !LoadCellReconcileSettlement(database_, old),
              "operation_journal_request_conflict",
              "old Apply release replay differs from success");
    }
    Require(generation == static_cast<std::int64_t>(resulting.value()),
            "operation_journal_request_conflict", "old Apply release replay generation changed");
    transaction.Commit();
    return resulting;
  }
  Require(durable_held == expected_held_claims &&
              generation == static_cast<std::int64_t>(expected_admission_generation.value()),
          "operation_journal_request_conflict",
          "old Apply release inventory or generation changed");
  for (const auto& old : expected_held_claims) {
    if (old == fresh_claim) continue;
    Statement update(
        database_,
        "UPDATE cell_admission_claims SET release_owner_role=?,release_owner_epoch=?,"
        "release_owner_transaction_id=?,release_request_id=?,release_request_payload_digest=?,"
        "release_acquired_admission_generation=?,release_settlement_kind=?,release_evidence_digest="
        "? "
        "WHERE claim_generation=? AND release_owner_role IS NULL AND reconciled_sequence IS NULL");
    update.Bind(1, to_string(old.intent.owner_role));
    update.Bind(2, old.intent.owner_epoch.value());
    update.Bind(3, old.intent.owner_transaction_id);
    update.Bind(4, old.intent.request_id.value());
    update.Bind(5, old.intent.request_payload_digest.value());
    update.Bind(6, static_cast<std::int64_t>(old.acquired_admission_generation.value()));
    update.Bind(7, to_string(fresh_success_release.settlement.settlement_kind));
    update.Bind(8, fresh_success_release.settlement.evidence_digest.value());
    update.Bind(9, static_cast<std::int64_t>(old.claim_generation.value()));
    Require(!update.Step() && sqlite3_changes(database_) == 1, "operation_journal_corrupt",
            "old Apply success release did not update exact row");
  }
  if (has_old) {
    Statement advance(database_,
                      "UPDATE journal_metadata SET cell_admission_generation_high_water=? "
                      "WHERE singleton=1 AND cell_admission_generation_high_water=?");
    advance.Bind(1, static_cast<std::int64_t>(resulting.value()));
    advance.Bind(2, static_cast<std::int64_t>(expected_admission_generation.value()));
    Require(!advance.Step() && sqlite3_changes(database_) == 1, "operation_journal_record_invalid",
            "old Apply success release generation changed");
  }
  CommitConfigurationApply(transaction, database_, fresh_claim, [&] {
    const auto held = LoadHeldCellClaims(database_);
    const auto owners = LoadCellCurrentOwners(database_);
    const auto observed_generation = ScalarInteger(
        database_,
        "SELECT cell_admission_generation_high_water FROM journal_metadata WHERE singleton=1");
    bool all_released = true;
    bool all_absent = true;
    for (const auto& old : expected_held_claims) {
      if (old == fresh_claim) continue;
      const auto row = LoadCellClaim(database_, old.claim_generation.value());
      const CellAdmissionClaimReleaseDTO release{old.intent.owner_role,
                                                 old.intent.owner_epoch,
                                                 old.intent.owner_transaction_id,
                                                 old.intent.request_id,
                                                 old.intent.request_payload_digest,
                                                 old.acquired_admission_generation,
                                                 old.claim_generation,
                                                 fresh_success_release.settlement};
      all_released = all_released && row.claim == old && row.release == release &&
                     !LoadCellReconcileSettlement(database_, old);
      all_absent = all_absent && row.claim == old && !row.release &&
                   !LoadCellReconcileSettlement(database_, old);
    }
    if (held == std::vector<CellAdmissionClaimDTO>{fresh_claim} && all_released &&
        owners == expected_current_owners &&
        observed_generation == static_cast<std::int64_t>(resulting.value())) {
      return ConfigurationCommitReadback::kCommitted;
    }
    if (held == expected_held_claims && all_absent && owners == expected_current_owners &&
        observed_generation == static_cast<std::int64_t>(expected_admission_generation.value())) {
      return ConfigurationCommitReadback::kAbsent;
    }
    return ConfigurationCommitReadback::kUnknown;
  });
  return resulting;
}

std::uint64_t SqliteOperationJournal::Impl::RecordCellAdmissionClaimsReconciled(
    const RequestOwnerEpoch& owner_epoch, const CellAdmissionReconcileDTO& reconcile,
    const CellAdmissionReconcileValidationDTO& validation,
    const AdmissionGeneration resulting_generation) {
  Require(reconcile.kernel_owner_epoch == owner_epoch && resulting_generation.nonzero() &&
              resulting_generation.value() <= kMaximumSafeInteger &&
              reconcile.retained_claims.size() <= kMaximumCellAdmissionClaims &&
              reconcile.current_owner_epochs.size() <= kMaximumCellAdmissionClaims &&
              std::is_sorted(reconcile.retained_claims.begin(), reconcile.retained_claims.end(),
                             [](const auto& left, const auto& right) {
                               return left.claim_generation < right.claim_generation;
                             }) &&
              std::adjacent_find(reconcile.retained_claims.begin(), reconcile.retained_claims.end(),
                                 [](const auto& left, const auto& right) {
                                   return left.claim_generation == right.claim_generation;
                                 }) == reconcile.retained_claims.end() &&
              std::is_sorted(reconcile.current_owner_epochs.begin(),
                             reconcile.current_owner_epochs.end(),
                             [](const auto& left, const auto& right) {
                               return std::tie(left.owner_role, left.owner_epoch) <
                                      std::tie(right.owner_role, right.owner_epoch);
                             }) &&
              std::adjacent_find(reconcile.current_owner_epochs.begin(),
                                 reconcile.current_owner_epochs.end(),
                                 [](const auto& left, const auto& right) {
                                   return left.owner_role == right.owner_role;
                                 }) == reconcile.current_owner_epochs.end() &&
              validation.current_owner_epochs == reconcile.current_owner_epochs &&
              validation.settlements.size() <= reconcile.retained_claims.size() &&
              std::is_sorted(validation.settlements.begin(), validation.settlements.end(),
                             [](const auto& left, const auto& right) {
                               return left.predecessor_claim.claim_generation <
                                      right.predecessor_claim.claim_generation;
                             }) &&
              std::adjacent_find(validation.settlements.begin(), validation.settlements.end(),
                                 [](const auto& left, const auto& right) {
                                   return left.predecessor_claim.claim_generation ==
                                          right.predecessor_claim.claim_generation;
                                 }) == validation.settlements.end() &&
              content_identity::is_sha256_digest(reconcile.reconciliation_evidence_digest.value()),
          "operation_journal_record_invalid", "cell-admission reconcile record is invalid");
  std::string inventory;
  for (const auto& claim : reconcile.retained_claims) {
    auto digest = CellAdmissionClaimIntentDigest(claim.intent);
    Require(digest.ok(), "operation_journal_record_invalid",
            "cell-admission reconcile claim is invalid");
    inventory += std::to_string(claim.claim_generation.value()) + ":" +
                 std::to_string(claim.acquired_admission_generation.value()) + ":" +
                 std::to_string(claim.accepted_at_ns) + ":" + digest.value().value() + "\n";
  }
  const auto inventory_digest = content_identity::sha256_digest(inventory);
  std::string current_owners;
  for (const auto& epoch : reconcile.current_owner_epochs) {
    Require(!epoch.owner_epoch.empty() && !to_string(epoch.owner_role).empty(),
            "operation_journal_record_invalid", "cell-admission reconcile owner epoch is invalid");
    current_owners +=
        std::string(to_string(epoch.owner_role)) + ":" + epoch.owner_epoch.value() + "\n";
  }
  const auto current_owners_digest = content_identity::sha256_digest(current_owners);
  std::string authorizations;
  for (const auto& authorization : validation.settlements) {
    auto predecessor_digest =
        CellAdmissionClaimIntentDigest(authorization.predecessor_claim.intent);
    Require(predecessor_digest.ok() &&
                authorization.successor_owner_role ==
                    authorization.predecessor_claim.intent.owner_role &&
                authorization.successor_owner_epoch !=
                    authorization.predecessor_claim.intent.owner_epoch &&
                authorization.settlement_predicate == CellAdmissionSettlementKindDTO::kReconciled &&
                authorization.reconciliation_evidence_digest ==
                    reconcile.reconciliation_evidence_digest,
            "operation_journal_record_invalid",
            "cell-admission settlement authorization is invalid");
    authorizations +=
        std::to_string(authorization.predecessor_claim.claim_generation.value()) + ":" +
        std::to_string(authorization.predecessor_claim.acquired_admission_generation.value()) +
        ":" + std::to_string(authorization.predecessor_claim.accepted_at_ns) + ":" +
        predecessor_digest.value().value() + ":" +
        std::string(to_string(authorization.successor_owner_role)) + ":" +
        authorization.successor_owner_epoch.value() + ":" +
        std::string(to_string(authorization.settlement_predicate)) + ":" +
        authorization.reconciliation_evidence_digest.value() + "\n";
  }
  const auto authorization_digest = content_identity::sha256_digest(authorizations);
  const auto lock = LockOrdinaryWriter();
  RequireOpen();
  Transaction transaction(database_);
  Statement active(database_, "SELECT COUNT(*) FROM owner_epochs WHERE owner_epoch=? AND active=1");
  active.Bind(1, owner_epoch.value());
  Require(active.Step() && active.Integer(0) == 1 && !active.Step(),
          "operation_journal_owner_epoch_stale",
          "cell-admission reconcile names a non-active Kernel owner epoch");
  Statement replay(
      database_,
      "SELECT reconcile_sequence,evidence_digest,inventory_digest,current_owners_digest,"
      "authorization_digest,resulting_generation "
      "FROM cell_admission_reconciliations WHERE kernel_owner_epoch=? "
      "AND evidence_digest=?");
  replay.Bind(1, owner_epoch.value());
  replay.Bind(2, reconcile.reconciliation_evidence_digest.value());
  if (replay.Step()) {
    const auto sequence = PositiveIdentifier(replay.Integer(0), "reconcile sequence");
    Require(replay.Text(1) == reconcile.reconciliation_evidence_digest.value() &&
                replay.Text(2) == inventory_digest && replay.Text(3) == current_owners_digest &&
                replay.Text(4) == authorization_digest &&
                replay.Integer(5) == static_cast<std::int64_t>(resulting_generation.value()) &&
                !replay.Step(),
            "operation_journal_request_conflict",
            "cell-admission reconcile replay conflicts with durable truth");
    transaction.Commit();
    return sequence;
  }
  std::vector<CellAdmissionClaimDTO> durable_held;
  Statement held(database_,
                 "SELECT claim_generation FROM cell_admission_claims "
                 "WHERE release_owner_role IS NULL AND reconciled_sequence IS NULL "
                 "ORDER BY claim_generation");
  while (held.Step()) {
    durable_held.push_back(
        LoadCellClaim(database_,
                      PositiveIdentifier(held.Integer(0), "cell-admission claim generation"))
            .claim);
  }
  Require(durable_held == reconcile.retained_claims, "operation_journal_request_conflict",
          "cell-admission reconcile inventory is not complete exact durable held truth");
  for (const auto& authorization : validation.settlements) {
    Require(std::find(durable_held.begin(), durable_held.end(), authorization.predecessor_claim) !=
                durable_held.end(),
            "operation_journal_request_conflict",
            "cell-admission settlement authorization does not bind an exact predecessor");
    Require(
        std::find(validation.current_owner_epochs.begin(), validation.current_owner_epochs.end(),
                  CellAdmissionCurrentOwnerEpochDTO{authorization.successor_owner_role,
                                                    authorization.successor_owner_epoch}) !=
            validation.current_owner_epochs.end(),
        "operation_journal_request_conflict",
        "cell-admission settlement successor is not independently authorized");
  }
  std::vector<CellAdmissionCurrentOwnerEpochDTO> durable_current_owners;
  Statement durable_owners(database_,
                           "SELECT owner_role,owner_epoch FROM cell_admission_current_owners");
  while (durable_owners.Step()) {
    durable_current_owners.push_back(
        {CellOwnerRole(durable_owners.Text(0)), RequestOwnerEpoch(durable_owners.Text(1))});
  }
  std::sort(durable_current_owners.begin(), durable_current_owners.end(),
            [](const auto& left, const auto& right) { return left.owner_role < right.owner_role; });
  for (const auto& durable_owner : durable_current_owners) {
    Require(
        std::any_of(
            validation.current_owner_epochs.begin(), validation.current_owner_epochs.end(),
            [&](const auto& current) { return current.owner_role == durable_owner.owner_role; }),
        "operation_journal_request_conflict",
        "cell-admission reconciliation omitted durable current-owner truth");
  }
  const bool owners_changed = durable_current_owners != validation.current_owner_epochs;
  const bool changed = owners_changed || !validation.settlements.empty();
  Statement current_generation(
      database_,
      "SELECT cell_admission_generation_high_water FROM journal_metadata WHERE singleton=1");
  Require(current_generation.Step(), "operation_journal_corrupt",
          "cell-admission reconciliation generation is missing");
  const auto prior_generation = current_generation.Integer(0);
  Require(!current_generation.Step() && prior_generation > 0 &&
              resulting_generation.value() ==
                  static_cast<std::uint64_t>(prior_generation) + (changed ? 1U : 0U),
          "operation_journal_record_invalid",
          "cell-admission reconciliation generation is not exact");
  Statement insert(database_,
                   "INSERT INTO cell_admission_reconciliations("
                   "kernel_owner_epoch,evidence_digest,inventory_digest,current_owners_digest,"
                   "authorization_digest,resulting_generation) VALUES(?,?,?,?,?,?)");
  insert.Bind(1, owner_epoch.value());
  insert.Bind(2, reconcile.reconciliation_evidence_digest.value());
  insert.Bind(3, inventory_digest);
  insert.Bind(4, current_owners_digest);
  insert.Bind(5, authorization_digest);
  insert.Bind(6, static_cast<std::int64_t>(resulting_generation.value()));
  Require(!insert.Step(), "operation_journal_corrupt",
          "cell-admission reconcile insert returned an unexpected row");
  const auto sequence =
      PositiveIdentifier(sqlite3_last_insert_rowid(database_), "reconcile sequence");
  for (std::size_t index = 0; index < validation.settlements.size(); ++index) {
    const auto& authorization = validation.settlements[index];
    auto predecessor_digest =
        CellAdmissionClaimIntentDigest(authorization.predecessor_claim.intent);
    Statement evidence(
        database_,
        "INSERT INTO cell_admission_reconcile_settlements("
        "reconcile_sequence,settlement_index,claim_generation,predecessor_intent_digest,"
        "predecessor_owner_role,predecessor_owner_epoch,successor_owner_role,"
        "successor_owner_epoch,settlement_predicate,evidence_digest) "
        "VALUES(?,?,?,?,?,?,?,?,?,?)");
    evidence.Bind(1, static_cast<std::int64_t>(sequence));
    evidence.Bind(2, static_cast<std::int64_t>(index));
    evidence.Bind(
        3, static_cast<std::int64_t>(authorization.predecessor_claim.claim_generation.value()));
    evidence.Bind(4, predecessor_digest.value().value());
    evidence.Bind(5, to_string(authorization.predecessor_claim.intent.owner_role));
    evidence.Bind(6, authorization.predecessor_claim.intent.owner_epoch.value());
    evidence.Bind(7, to_string(authorization.successor_owner_role));
    evidence.Bind(8, authorization.successor_owner_epoch.value());
    evidence.Bind(9, to_string(authorization.settlement_predicate));
    evidence.Bind(10, authorization.reconciliation_evidence_digest.value());
    Require(!evidence.Step(), "operation_journal_corrupt",
            "cell-admission settlement evidence insert returned an unexpected row");
    Statement settle(database_,
                     "UPDATE cell_admission_claims SET reconciled_sequence=? "
                     "WHERE claim_generation=? AND release_owner_role IS NULL "
                     "AND reconciled_sequence IS NULL");
    settle.Bind(1, static_cast<std::int64_t>(sequence));
    settle.Bind(
        2, static_cast<std::int64_t>(authorization.predecessor_claim.claim_generation.value()));
    Require(!settle.Step() && sqlite3_changes(database_) == 1, "operation_journal_corrupt",
            "cell-admission reconciliation did not settle exact predecessor");
  }
  for (const auto& current : validation.current_owner_epochs) {
    const auto prior =
        std::find_if(durable_current_owners.begin(), durable_current_owners.end(),
                     [&](const auto& owner) { return owner.owner_role == current.owner_role; });
    if (prior != durable_current_owners.end() && prior->owner_epoch != current.owner_epoch) {
      Statement predecessor_epoch(database_,
                                  "SELECT close_sequence FROM owner_epochs WHERE active=0 "
                                  "ORDER BY epoch_sequence DESC LIMIT 1");
      const bool predecessor_clean = predecessor_epoch.Step() && !predecessor_epoch.IsNull(0);
      Require(!predecessor_epoch.Step(), "operation_journal_corrupt",
              "cell-admission predecessor epoch query returned multiple rows");
      const bool successor_settlement = std::any_of(
          validation.settlements.begin(), validation.settlements.end(),
          [&](const auto& authorization) {
            return authorization.predecessor_claim.intent.owner_role == current.owner_role &&
                   authorization.predecessor_claim.intent.owner_epoch == prior->owner_epoch &&
                   authorization.successor_owner_epoch == current.owner_epoch;
          });
      Statement unresolved(database_,
                           "SELECT COUNT(*) FROM cell_admission_claims "
                           "WHERE owner_role=? AND owner_epoch=? AND release_owner_role IS NULL "
                           "AND reconciled_sequence IS NULL");
      unresolved.Bind(1, to_string(current.owner_role));
      unresolved.Bind(2, prior->owner_epoch.value());
      Require(unresolved.Step() && unresolved.Integer(0) == 0 && !unresolved.Step(),
              "operation_journal_owner_close_blocked",
              "unresolved claims block request-owner epoch rotation");
      // The CCLC and Composer validators independently prove that their exact
      // successor journal epoch is active and that no owner-local transaction
      // or physical effect remains unresolved.  The Program-cycle validator
      // provides the same proof for an idle or exactly released cycle, and the
      // Kernel validates its own manual-operation successor against the exact
      // retained claim inventory.  Those proofs can authorize an empty-claim
      // rotation after an unclean Kernel-host epoch; every other request-owner
      // role still requires a clean close or an exact held-claim successor
      // settlement.
      const bool empty_validated_rotation =
          (current.owner_role == CellAdmissionOwnerRoleDTO::kAuthoringOwner ||
           current.owner_role == CellAdmissionOwnerRoleDTO::kProgramRunner ||
           current.owner_role == CellAdmissionOwnerRoleDTO::kManualOperationOwner) &&
          validation.settlements.empty();
      Require(predecessor_clean || successor_settlement || empty_validated_rotation,
              "operation_journal_owner_close_blocked",
              "unclean owner replacement cannot retire request membership "
              "without exact successor settlement");
      if (empty_validated_rotation) {
        Statement compact_validated_membership(
            database_,
            "DELETE FROM cell_admission_request_membership "
            "WHERE claim_generation IN ("
            "SELECT claim_generation FROM cell_admission_claims "
            "WHERE owner_role=? AND owner_epoch=? "
            "AND (release_owner_role IS NOT NULL OR reconciled_sequence IS NOT NULL))");
        compact_validated_membership.Bind(1, to_string(current.owner_role));
        compact_validated_membership.Bind(2, prior->owner_epoch.value());
        Require(!compact_validated_membership.Step(), "operation_journal_corrupt",
                "validated obsolete request membership retirement failed");
      }
      Statement compact_reconciled_membership(
          database_,
          "DELETE FROM cell_admission_request_membership "
          "WHERE claim_generation IN ("
          "SELECT claim_generation FROM cell_admission_reconcile_settlements "
          "WHERE successor_owner_role=? AND successor_owner_epoch=?)");
      compact_reconciled_membership.Bind(1, to_string(current.owner_role));
      compact_reconciled_membership.Bind(2, prior->owner_epoch.value());
      Require(!compact_reconciled_membership.Step(), "operation_journal_corrupt",
              "obsolete reconciled cell-admission membership retirement failed");
      Statement compact_reconciled_claims(
          database_,
          "DELETE FROM cell_admission_claims WHERE claim_generation IN ("
          "SELECT claim_generation FROM cell_admission_reconcile_settlements "
          "WHERE successor_owner_role=? AND successor_owner_epoch=?)");
      compact_reconciled_claims.Bind(1, to_string(current.owner_role));
      compact_reconciled_claims.Bind(2, prior->owner_epoch.value());
      Require(!compact_reconciled_claims.Step(), "operation_journal_corrupt",
              "obsolete reconciled cell-admission claim retirement failed");
      Statement compact_reconcile_settlements(
          database_,
          "DELETE FROM cell_admission_reconcile_settlements "
          "WHERE successor_owner_role=? AND successor_owner_epoch=?");
      compact_reconcile_settlements.Bind(1, to_string(current.owner_role));
      compact_reconcile_settlements.Bind(2, prior->owner_epoch.value());
      Require(!compact_reconcile_settlements.Step(), "operation_journal_corrupt",
              "obsolete cell-admission reconcile settlement retirement failed");
      Statement compact_claims(database_,
                               "DELETE FROM cell_admission_claims "
                               "WHERE owner_role=? AND owner_epoch=? "
                               "AND release_owner_role IS NOT NULL");
      compact_claims.Bind(1, to_string(current.owner_role));
      compact_claims.Bind(2, prior->owner_epoch.value());
      Require(!compact_claims.Step(), "operation_journal_corrupt",
              "cell-admission stale claim retirement failed");
      Statement compact_namespace(database_,
                                  "DELETE FROM cell_admission_request_membership "
                                  "WHERE owner_role=? AND owner_epoch=? "
                                  "AND claim_generation NOT IN ("
                                  "SELECT claim_generation FROM cell_admission_claims)");
      compact_namespace.Bind(1, to_string(current.owner_role));
      compact_namespace.Bind(2, prior->owner_epoch.value());
      Require(!compact_namespace.Step(), "operation_journal_corrupt",
              "cell-admission stale namespace retirement failed");
    }
    Statement publish_owner(
        database_,
        "INSERT INTO cell_admission_current_owners("
        "owner_role,owner_epoch,established_kernel_owner_epoch) VALUES(?,?,?) "
        "ON CONFLICT(owner_role) DO UPDATE SET owner_epoch=excluded.owner_epoch,"
        "established_kernel_owner_epoch=excluded.established_kernel_owner_epoch");
    publish_owner.Bind(1, to_string(current.owner_role));
    publish_owner.Bind(2, current.owner_epoch.value());
    publish_owner.Bind(3, owner_epoch.value());
    Require(!publish_owner.Step(), "operation_journal_corrupt",
            "cell-admission current-owner publication failed");
  }
  Statement compact_reconciliations(
      database_,
      "DELETE FROM cell_admission_reconciliations "
      "WHERE reconcile_sequence<>? "
      "AND NOT EXISTS (SELECT 1 FROM cell_admission_reconcile_settlements settlement "
      "WHERE settlement.reconcile_sequence="
      "cell_admission_reconciliations.reconcile_sequence)");
  compact_reconciliations.Bind(1, static_cast<std::int64_t>(sequence));
  Require(!compact_reconciliations.Step(), "operation_journal_corrupt",
          "cell-admission stale reconciliation retirement failed");
  if (changed) {
    Statement advance(database_,
                      "UPDATE journal_metadata SET cell_admission_generation_high_water=? "
                      "WHERE singleton=1 AND cell_admission_generation_high_water=?");
    advance.Bind(1, static_cast<std::int64_t>(resulting_generation.value()));
    advance.Bind(2, prior_generation);
    Require(!advance.Step() && sqlite3_changes(database_) == 1, "operation_journal_record_invalid",
            "cell-admission reconciliation generation did not advance");
  }
  transaction.Commit();
  return sequence;
}

std::uint64_t SqliteOperationJournal::Impl::RecordCellAdmissionGenerationAdvanced(
    const RequestOwnerEpoch& owner_epoch, const AdmissionGeneration prior_generation,
    const AdmissionGeneration resulting_generation) {
  Require(prior_generation.nonzero() && prior_generation.value() < kMaximumSafeInteger &&
              resulting_generation.value() == prior_generation.value() + 1U,
          "operation_journal_record_invalid", "cell-admission generation transition is invalid");
  const auto lock = LockOrdinaryWriter();
  RequireOpen();
  Transaction transaction(database_);
  Statement active(database_, "SELECT COUNT(*) FROM owner_epochs WHERE owner_epoch=? AND active=1");
  active.Bind(1, owner_epoch.value());
  Require(active.Step() && active.Integer(0) == 1 && !active.Step(),
          "operation_journal_owner_epoch_stale",
          "cell-admission generation transition names a non-active Kernel owner epoch");
  Statement advance(database_,
                    "UPDATE journal_metadata SET cell_admission_generation_high_water=? "
                    "WHERE singleton=1 AND cell_admission_generation_high_water=?");
  advance.Bind(1, static_cast<std::int64_t>(resulting_generation.value()));
  advance.Bind(2, static_cast<std::int64_t>(prior_generation.value()));
  Require(!advance.Step() && sqlite3_changes(database_) == 1, "operation_journal_record_invalid",
          "cell-admission generation transition is not the durable successor");
  transaction.Commit();
  return resulting_generation.value();
}

std::uint64_t SqliteOperationJournal::Impl::RecordInterruptRuleStateChanged(
    const IoRuleStateChangeDTO& change) {
  Require(!io_rule_state_field_error(change.context, change.rule) && change.occurred_at_ns >= 0,
          "operation_journal_record_invalid", "global rule-state projection is invalid");
  const auto& context = change.context;
  CanonicalValue acquisition(nullptr);
  if (change.rule.triggering_acquisition) {
    const auto& actual = *change.rule.triggering_acquisition;
    const auto& cursor = actual.source_cursor;
    acquisition = CanonicalValue(CanonicalValue::Object{
        {"source_id", CanonicalValue(actual.source_id.value())},
        {"source_epoch", CanonicalValue(actual.incarnation.value())},
        {"source_sequence", CanonicalValue(std::to_string(actual.source_sequence))},
        {"source_cursor",
         CanonicalValue(CanonicalValue::Object{
             {"owner_epoch", CanonicalValue(cursor.owner_epoch.value())},
             {"catalog_generation", CanonicalValue(std::to_string(cursor.catalog_generation))},
             {"source_id", CanonicalValue(cursor.source_id.value())},
             {"record_sequence", CanonicalValue(std::to_string(cursor.record_sequence))}})}});
  }
  CanonicalValue::Object fields{
      {"owner_epoch", CanonicalValue(context.owner_epoch.value())},
      {"catalog_generation", CanonicalValue(std::to_string(context.catalog_generation))},
      {"active_publication_generation",
       CanonicalValue(static_cast<double>(context.active_publication_generation.value()))},
      {"retained_bundle_id", CanonicalValue(context.retained_bundle_id.value())},
      {"graph_revision", CanonicalValue(context.graph_revision.value())},
      {"rule_id", CanonicalValue(change.rule.rule_id.value())},
      {"state", CanonicalValue(std::string(io_global_rule_state_token(change.rule.state)))},
      {"triggering_acquisition", std::move(acquisition)}};
  if (change.rule.unavailable_reason)
    fields.emplace_back("unavailable_reason", ErrorValue(change.rule.unavailable_reason));
  const auto payload = content_identity::canonical_json(CanonicalValue(std::move(fields)));
  Require(payload.size() <= kMaximumRecordJsonBytes, "operation_journal_record_too_large",
          "global rule-state projection exceeds the fixed payload bound");
  const auto digest = content_identity::sha256_digest(payload);
  const auto lock = LockOrdinaryWriter();
  RequireOpen();
  const auto occurred_at_wall = wall_time_text_();
  ValidateWallTimeText(occurred_at_wall);
  Transaction transaction(database_);
  if (critical_section_observer_) critical_section_observer_("ordinary_write");
  Statement active(database_, "SELECT COUNT(*) FROM owner_epochs WHERE owner_epoch=? AND active=1");
  active.Bind(1, context.owner_epoch.value());
  Require(active.Step() && active.Integer(0) == 1 && !active.Step(),
          "operation_journal_owner_epoch_stale", "global rule-state owner is not active");
  Statement insert(
      database_,
      "INSERT INTO journal_records(owner_epoch,record_kind,request_id,operation_id,"
      "operation_revision,occurred_at_ns,occurred_at_wall,effect_pending,payload_json,"
      "payload_digest) VALUES(?,'interrupt_rule_state_changed',NULL,NULL,NULL,?,?,0,?,?)");
  insert.Bind(1, context.owner_epoch.value());
  insert.Bind(2, change.occurred_at_ns);
  insert.Bind(3, occurred_at_wall);
  insert.Bind(4, payload);
  insert.Bind(5, digest);
  Require(!insert.Step(), "operation_journal_corrupt", "global rule-state insert returned a row");
  const auto sequence =
      PositiveIdentifier(sqlite3_last_insert_rowid(database_), "operation journal sequence");
  // Reuse this journal's existing retained-history bound. Current truth comes
  // from the atomic Kernel baseline, never from assuming old rows still exist.
  Statement compact(
      database_,
      "DELETE FROM journal_records WHERE owner_epoch=? AND "
      "record_kind='interrupt_rule_state_changed' "
      "AND sequence NOT IN (SELECT sequence FROM journal_records WHERE owner_epoch=? "
      "AND record_kind='interrupt_rule_state_changed' ORDER BY sequence DESC LIMIT ?)");
  compact.Bind(1, context.owner_epoch.value());
  compact.Bind(2, context.owner_epoch.value());
  compact.Bind(3, static_cast<std::int64_t>(history_capacity_));
  Require(!compact.Step(), "operation_journal_corrupt",
          "global rule-state compaction returned a row");
  transaction.Commit();
  return sequence;
}

// Active TCP (M7, design 4): one confirmed Active TCP change. Sparse and
// owner-epoch scoped like the rule-state rows; current truth is the robot
// owner's baseline (with this row's sequence), never old rows still kept.
std::uint64_t SqliteOperationJournal::Impl::RecordActiveTcpChanged(
    const RequestOwnerEpoch& owner_epoch, const ActiveTcpChangeDTO& change) {
  Require(!owner_epoch.empty() && !change.component_id.empty() &&
              (!change.entry || valid_tcp_entry(*change.entry)) &&
              (!change.previous || valid_tcp_entry(*change.previous)) &&
              change.registration_generation != 0U &&
              change.occurred_at_ns >= 0,
          "operation_journal_record_invalid", "Active TCP change is invalid");
  const auto entry_value = [](const std::optional<TcpEntryDTO>& entry) {
    return entry ? EncodeTcpEntry(*entry) : CanonicalValue(nullptr);
  };
  const auto payload = content_identity::canonical_json(CanonicalValue(CanonicalValue::Object{
      {"api_version", CanonicalValue("manyforge.active_tcp_change.v1")},
      {"owner_epoch", CanonicalValue(owner_epoch.value())},
      {"component_id", CanonicalValue(change.component_id.value())},
      {"previous", entry_value(change.previous)},
      {"entry", entry_value(change.entry)},
      {"source", CanonicalValue(std::string(to_string(change.source)))},
      {"offset_sent", CanonicalValue(change.offset_sent)},
      {"registration_generation",
       CanonicalValue(static_cast<double>(change.registration_generation))},
      {"sequence", CanonicalValue(static_cast<double>(change.sequence))}}));
  Require(payload.size() <= kMaximumRecordJsonBytes, "operation_journal_record_too_large",
          "Active TCP change exceeds the fixed payload bound");
  const auto digest = content_identity::sha256_digest(payload);
  const auto lock = LockOrdinaryWriter();
  RequireOpen();
  const auto occurred_at_wall = wall_time_text_();
  ValidateWallTimeText(occurred_at_wall);
  Transaction transaction(database_);
  if (critical_section_observer_) critical_section_observer_("ordinary_write");
  Statement active(database_, "SELECT COUNT(*) FROM owner_epochs WHERE owner_epoch=? AND active=1");
  active.Bind(1, owner_epoch.value());
  Require(active.Step() && active.Integer(0) == 1 && !active.Step(),
          "operation_journal_owner_epoch_stale", "Active TCP change owner is not active");
  Statement insert(
      database_,
      "INSERT INTO journal_records(owner_epoch,record_kind,request_id,operation_id,"
      "operation_revision,occurred_at_ns,occurred_at_wall,effect_pending,payload_json,"
      "payload_digest) VALUES(?,'active_tcp_changed',NULL,NULL,NULL,?,?,0,?,?)");
  insert.Bind(1, owner_epoch.value());
  insert.Bind(2, change.occurred_at_ns);
  insert.Bind(3, occurred_at_wall);
  insert.Bind(4, payload);
  insert.Bind(5, digest);
  Require(!insert.Step(), "operation_journal_corrupt", "Active TCP change insert returned a row");
  const auto sequence =
      PositiveIdentifier(sqlite3_last_insert_rowid(database_), "operation journal sequence");
  // The journal's existing retained-history bound, as for rule-state rows.
  Statement compact(
      database_,
      "DELETE FROM journal_records WHERE owner_epoch=? AND record_kind='active_tcp_changed' "
      "AND sequence NOT IN (SELECT sequence FROM journal_records WHERE owner_epoch=? "
      "AND record_kind='active_tcp_changed' ORDER BY sequence DESC LIMIT ?)");
  compact.Bind(1, owner_epoch.value());
  compact.Bind(2, owner_epoch.value());
  compact.Bind(3, static_cast<std::int64_t>(history_capacity_));
  Require(!compact.Step(), "operation_journal_corrupt",
          "Active TCP change compaction returned a row");
  transaction.Commit();
  return sequence;
}

SqliteOperationJournal::SqliteOperationJournal(
    std::filesystem::path database_path, CellId cell_id, std::size_t history_capacity,
    std::function<std::string()> wall_time_text,
    std::function<void(std::string_view)> critical_section_observer)
    : impl_(std::make_unique<Impl>(std::move(database_path), std::move(cell_id), history_capacity,
                                   std::move(wall_time_text),
                                   std::move(critical_section_observer))) {}

SqliteOperationJournal::~SqliteOperationJournal() = default;

Result<OperationJournalRecoveryDTO> SqliteOperationJournal::BeginOwnerEpoch(
    const RequestOwnerEpoch& owner_epoch, std::int64_t started_at_ns, std::size_t request_capacity,
    const std::vector<ResourceId>& resource_inventory) {
  return AsResult<OperationJournalRecoveryDTO>([&] {
    return impl_->BeginOwnerEpoch(owner_epoch, started_at_ns, request_capacity, resource_inventory);
  });
}

Result<std::uint64_t> SqliteOperationJournal::RecordOperationAccepted(
    const RequestOwnerEpoch& owner_epoch, const OperationIntentDTO& intent,
    const OperationAcceptedDTO& accepted, const OperationStateDTO& state,
    const std::span<const OperationId> protected_operation_ids) {
  return AsResult<std::uint64_t>([&] {
    return impl_->RecordOperationAccepted(owner_epoch, intent, accepted, state,
                                          protected_operation_ids);
  });
}

Result<std::uint64_t> SqliteOperationJournal::RecordControlAccepted(
    const RequestOwnerEpoch& owner_epoch, const OperationControlRequestDTO& request,
    const OperationControlAcceptedDTO& accepted, const OperationStateDTO& state) {
  return AsResult<std::uint64_t>(
      [&] { return impl_->RecordControlAccepted(owner_epoch, request, accepted, state); });
}

EmergencyStopAcceptanceProjectionResultDTO SqliteOperationJournal::RecordStopAccepted(
    const RequestOwnerEpoch& owner_epoch, const EmergencyStopAcceptanceRecordDTO& acceptance,
    const EmergencyStopProjectedIntentDTO& intent, const StopAcceptedDTO& accepted,
    const EmergencyStopAffectedStateSnapshotDTO& affected_state) noexcept {
  return AsStopProjectionResult([&] {
    return impl_->RecordStopAccepted(owner_epoch, acceptance, intent, accepted, affected_state);
  });
}

StopForensicProjectionResultDTO SqliteOperationJournal::RecordStopTerminal(
    const RequestOwnerEpoch& owner_epoch, const StopStatusDTO& stop_status,
    const StopCertificateDTO& certificate) noexcept {
  return AsStopForensicProjectionResult(
      [&] { return impl_->RecordStopTerminal(owner_epoch, stop_status, certificate); });
}

Result<std::uint64_t> SqliteOperationJournal::CloseOwnerEpoch(const RequestOwnerEpoch& owner_epoch,
                                                              const std::int64_t closed_at_ns) {
  return AsResult<std::uint64_t>([&] { return impl_->CloseOwnerEpoch(owner_epoch, closed_at_ns); });
}

Result<std::uint64_t> SqliteOperationJournal::RecordRuntimeScaleAccepted(
    const RequestOwnerEpoch& owner_epoch, const RuntimeScaleRequestDTO& request,
    const RuntimeScaleAcceptedDTO& accepted, const OperationStateDTO& state,
    const RuntimeScaleStatusDTO& scale_status) {
  return AsResult<std::uint64_t>([&] {
    return impl_->RecordRuntimeScaleAccepted(owner_epoch, request, accepted, state, scale_status);
  });
}

Result<std::uint64_t> SqliteOperationJournal::RecordOperationState(
    const RequestOwnerEpoch& owner_epoch, const OperationStateDTO& state,
    const RuntimeScaleStatusDTO* scale_status, const char* transition_reason,
    const std::span<const PreparedMotionDiagnosticsDTO> prepared_motion_diagnostics) {
  return AsResult<std::uint64_t>([&] {
    return impl_->RecordOperationState(owner_epoch, state, scale_status, transition_reason,
                                       prepared_motion_diagnostics);
  });
}

Result<std::uint64_t> SqliteOperationJournal::RecordCellAdmissionClaimAcquired(
    const RequestOwnerEpoch& owner_epoch, const CellAdmissionClaimDTO& claim) {
  return AsResult<std::uint64_t>(
      [&] { return impl_->RecordCellAdmissionClaimAcquired(owner_epoch, claim); });
}

Result<CellAdmissionClaimDTO> SqliteOperationJournal::RecordConfigurationApplyClaimAcquired(
    const RequestOwnerEpoch& owner_epoch, const CellAdmissionClaimDTO& claim,
    const CellAdmissionCurrentOwnerEpochDTO& verified_manager,
    const std::vector<CellAdmissionClaimDTO>& expected_held_claims,
    const std::vector<CellAdmissionCurrentOwnerEpochDTO>& expected_current_owners) {
  return AsResult<CellAdmissionClaimDTO>([&] {
    auto persisted = claim;
    impl_->RecordCellAdmissionClaimAcquired(owner_epoch, claim, &verified_manager,
                                            &expected_held_claims, &expected_current_owners,
                                            &persisted);
    return persisted;
  });
}

Result<AdmissionGeneration> SqliteOperationJournal::RecordTerminalApplyClaimsReleasedBySuccess(
    const RequestOwnerEpoch& owner_epoch, const CellAdmissionClaimDTO& fresh_claim,
    const CellAdmissionClaimReleaseDTO& fresh_success_release,
    const std::vector<CellAdmissionClaimDTO>& expected_held_claims,
    const std::vector<CellAdmissionCurrentOwnerEpochDTO>& expected_current_owners,
    const AdmissionGeneration expected_admission_generation) {
  return AsResult<AdmissionGeneration>([&] {
    return impl_->RecordTerminalApplyClaimsReleasedBySuccess(
        owner_epoch, fresh_claim, fresh_success_release, expected_held_claims,
        expected_current_owners, expected_admission_generation);
  });
}

Result<std::uint64_t> SqliteOperationJournal::RecordCellAdmissionClaimReleased(
    const RequestOwnerEpoch& owner_epoch, const CellAdmissionClaimDTO& claim,
    const CellAdmissionClaimReleaseDTO& release, const AdmissionGeneration resulting_generation) {
  return AsResult<std::uint64_t>([&] {
    return impl_->RecordCellAdmissionClaimReleased(owner_epoch, claim, release,
                                                   resulting_generation);
  });
}

Result<std::uint64_t> SqliteOperationJournal::RecordCellAdmissionClaimsReconciled(
    const RequestOwnerEpoch& owner_epoch, const CellAdmissionReconcileDTO& reconcile,
    const CellAdmissionReconcileValidationDTO& validation,
    const AdmissionGeneration resulting_generation) {
  return AsResult<std::uint64_t>([&] {
    return impl_->RecordCellAdmissionClaimsReconciled(owner_epoch, reconcile, validation,
                                                      resulting_generation);
  });
}

Result<std::uint64_t> SqliteOperationJournal::RecordCellAdmissionGenerationAdvanced(
    const RequestOwnerEpoch& owner_epoch, const AdmissionGeneration prior_generation,
    const AdmissionGeneration resulting_generation) {
  return AsResult<std::uint64_t>([&] {
    return impl_->RecordCellAdmissionGenerationAdvanced(owner_epoch, prior_generation,
                                                        resulting_generation);
  });
}

Result<std::uint64_t> SqliteOperationJournal::RecordActiveTcpChanged(
    const RequestOwnerEpoch& owner_epoch, const ActiveTcpChangeDTO& change) {
  return AsResult<std::uint64_t>(
      [&] { return impl_->RecordActiveTcpChanged(owner_epoch, change); });
}

Result<std::uint64_t> SqliteOperationJournal::RecordInterruptRuleStateChanged(
    const IoRuleStateChangeDTO& change) {
  return AsResult<std::uint64_t>([&] { return impl_->RecordInterruptRuleStateChanged(change); });
}

Result<OperationJournalEventPageDTO> SqliteOperationJournal::ReadEvents(
    const RequestOwnerEpoch& owner_epoch, const std::uint64_t after_sequence,
    const std::size_t limit) {
  return AsResult<OperationJournalEventPageDTO>(
      [&] { return impl_->ReadEvents(owner_epoch, after_sequence, limit); });
}

Result<OperationJournalEventPageDTO> SqliteOperationJournal::ReadOperationalEvents(
    const RequestOwnerEpoch& owner_epoch, const std::uint64_t after_sequence,
    const std::size_t limit) {
  return AsResult<OperationJournalEventPageDTO>(
      [&] { return impl_->ReadEvents(owner_epoch, after_sequence, limit, true); });
}

void SqliteOperationJournal::Close() {
  impl_->Close();
}

}  // namespace manyforge::core
