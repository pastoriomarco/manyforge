#include <manyforge_core/execution_event_change_signal.hpp>
#include <manyforge_core/io_rule_outcome_codec.hpp>
#include <sqlite3.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <deque>
#include <filesystem>
#include <functional>
#include <limits>
#include <manyforge_content_identity/canonical_value.hpp>
#include <manyforge_content_identity/content_identity.hpp>
#include <manyforge_core/resource_slot_table.hpp>
#include <manyforge_core/sqlite_operation_journal.hpp>
#include <manyforge_core/stop_event_codec.hpp>
#include <manyforge_core/tcp_selector_codec.hpp>
#include <manyforge_core/types/operation_member_id_json.hpp>
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
constexpr std::size_t kMaximumWallTimeBytes = 128U;
/// Records of one batch written per transaction (one COMMIT, one change signal).
constexpr std::size_t kMaximumTransactionRecords = 256U;
/// Finished Stop events whose rows the journal keeps (build spec section 6:
/// the newest 64). Safe because no owner reads a Stop row back: the Kernel
/// answers `read_stop` from its event ring, the runner reads Stop state from
/// the Kernel, and History has its own copy as soon as the event pump read the
/// row (it reads the journal continuously, in sequence order). An event is
/// finished when its completion row is written, or when more than
/// `request_capacity` (the Kernel's event ring size R) newer events exist: its
/// ring record is then overwritten and its completion row can never come
/// (design W4). Unfinished events keep their acceptance row, at most R of them.
constexpr std::size_t kMaximumRetainedFinishedStopEvents = 64U;
/// Released cell-admission claims whose two rows the journal keeps: the same
/// bound as the finished Stop events. Safe because no owner reads a claim row
/// back: the claim owner holds its claims in memory, the event pump never
/// delivers claim rows, and the launcher's next-start report looks only for
/// a claim without its released row, which is never deleted.
constexpr std::size_t kMaximumRetainedReleasedClaims = kMaximumRetainedFinishedStopEvents;

constexpr std::string_view kSchema = R"sql(
CREATE TABLE journal_metadata (
  singleton INTEGER PRIMARY KEY CHECK (singleton = 1),
  schema_id TEXT NOT NULL,
  schema_version INTEGER NOT NULL,
  cell_id TEXT NOT NULL,
  history_capacity INTEGER NOT NULL,
  application_start_id TEXT CHECK (application_start_id IS NULL OR
                                   length(application_start_id) = 16),
  started_at_ns INTEGER,
  request_capacity INTEGER,
  records_lost INTEGER NOT NULL DEFAULT 0 CHECK (records_lost IN (0, 1)),
  CHECK ((application_start_id IS NULL) = (started_at_ns IS NULL)),
  CHECK ((application_start_id IS NULL) = (request_capacity IS NULL))
) STRICT;
CREATE TABLE journal_records (
  sequence INTEGER PRIMARY KEY AUTOINCREMENT,
  record_kind TEXT NOT NULL,
  request_key BLOB CHECK (request_key IS NULL OR length(request_key) = 16),
  stop_event_id INTEGER,
  operation_id INTEGER,
  operation_revision INTEGER,
  occurred_at_ns INTEGER NOT NULL,
  occurred_at_wall TEXT NOT NULL,
  payload_json TEXT NOT NULL,
  payload_digest TEXT NOT NULL,
  CHECK ((operation_id IS NULL) = (operation_revision IS NULL)),
  CHECK ((record_kind IN ('stop_event_accepted','stop_event_completed')) =
         (stop_event_id IS NOT NULL)),
  CHECK (stop_event_id IS NULL OR
         (stop_event_id > 0 AND request_key IS NULL AND operation_id IS NULL))
) STRICT;
CREATE INDEX journal_stop_event_identity
  ON journal_records(stop_event_id, record_kind)
  WHERE stop_event_id IS NOT NULL;
CREATE INDEX journal_operation_sequence
  ON journal_records(operation_id, sequence);
CREATE TABLE operation_snapshots (
  operation_id INTEGER PRIMARY KEY,
  operation_revision INTEGER NOT NULL,
  program_binding_id TEXT NOT NULL,
  program_run_id INTEGER,
  coordination_policy TEXT NOT NULL,
  state TEXT NOT NULL,
  updated_at_ns INTEGER NOT NULL,
  stop_event_id INTEGER CHECK (stop_event_id IS NULL OR stop_event_id > 0),
  runtime_scale_generation INTEGER,
  snapshot_sequence INTEGER NOT NULL,
  journal_capture_order INTEGER NOT NULL DEFAULT 0 CHECK(journal_capture_order >= 0),
  FOREIGN KEY(snapshot_sequence) REFERENCES journal_records(sequence)
) STRICT;
CREATE INDEX operation_snapshot_sequence
  ON operation_snapshots(snapshot_sequence);
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

/// System time minus steady time, in nanoseconds: added to an action's
/// steady-clock reading it gives the action's wall time.
[[nodiscard]] std::int64_t SystemMinusSteadyNanoseconds() noexcept {
  const auto system = std::chrono::system_clock::now().time_since_epoch();
  const auto steady = std::chrono::steady_clock::now().time_since_epoch();
  return std::chrono::duration_cast<std::chrono::nanoseconds>(system).count() -
         std::chrono::duration_cast<std::chrono::nanoseconds>(steady).count();
}

[[nodiscard]] std::string WallTimeText(const std::int64_t system_ns) {
  const std::chrono::system_clock::time_point at{
      std::chrono::duration_cast<std::chrono::system_clock::duration>(
          std::chrono::nanoseconds(system_ns))};
  const auto seconds = std::chrono::floor<std::chrono::seconds>(at);
  const auto nanoseconds = system_ns - std::chrono::duration_cast<std::chrono::nanoseconds>(
                                           seconds.time_since_epoch())
                                           .count();
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

/// The typed error of the exception being handled (call only inside a catch).
[[nodiscard]] ErrorDTO CaughtError() {
  try {
    throw;
  } catch (const JournalFailure& error) {
    return JournalError(error);
  } catch (const std::exception& error) {
    return ErrorDTO{"operation_journal_internal_failure",
                    error.what(),
                    {{"reason", "operation_journal_exception"}}};
  }
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

  void Bind(int index, const OperationNumber& value) {
    Bind(index, static_cast<std::int64_t>(value.value()));
  }
  void Bind(int index, const ProgramRunNumber& value) {
    Bind(index, static_cast<std::int64_t>(value.value()));
  }
  /// Manual work has no run: NULL.
  void Bind(int index, const std::optional<ProgramRunNumber>& value) {
    if (value) {
      Bind(index, *value);
    } else {
      BindNull(index);
    }
  }
  void Bind(int index, double value) {
    Check(database_, sqlite3_bind_double(statement_, index, value), "bind SQLite real");
  }

  void BindNull(int index) {
    Check(database_, sqlite3_bind_null(statement_, index), "bind SQLite null");
  }

  /// A request key is stored as its 16 bytes.
  void Bind(int index, const RequestKey& key) {
    Check(database_,
          sqlite3_bind_blob(statement_, index, key.bytes.data(),
                            static_cast<int>(key.bytes.size()), SQLITE_TRANSIENT),
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

  [[nodiscard]] RequestKey Key(int column) const {
    Require(sqlite3_column_type(statement_, column) == SQLITE_BLOB &&
                sqlite3_column_bytes(statement_, column) == 16,
            "operation_journal_corrupt", "operation journal request key column is invalid");
    const auto* bytes = static_cast<const std::uint8_t*>(sqlite3_column_blob(statement_, column));
    RequestKey key;
    std::copy(bytes, bytes + 16, key.bytes.begin());
    return key;
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
    // After COMMIT: a woken event-stream reader sees this write.
    execution_event_change_signal().Notify();
  }

 private:
  sqlite3* database_{};
  bool committed_{};
};

/// One record inside a batch transaction: unless released, everything the
/// record wrote is rolled back and the transaction goes on.
class Savepoint final {
 public:
  explicit Savepoint(sqlite3* database) : database_(database) {
    Exec(database_, "SAVEPOINT journal_record");
  }

  ~Savepoint() {
    if (!released_) {
      sqlite3_exec(database_, "ROLLBACK TO journal_record", nullptr, nullptr, nullptr);
      sqlite3_exec(database_, "RELEASE journal_record", nullptr, nullptr, nullptr);
    }
  }
  Savepoint(const Savepoint&) = delete;
  Savepoint& operator=(const Savepoint&) = delete;

  void Release() {
    Exec(database_, "RELEASE journal_record");
    released_ = true;
  }

 private:
  sqlite3* database_{};
  bool released_{};
};

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

/// A run as its number; manual work has none.
[[nodiscard]] CanonicalValue RunValue(const std::optional<ProgramRunNumber>& run) {
  return run ? Number(run->value(), "Program run") : CanonicalValue(nullptr);
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

[[nodiscard]] CanonicalValue PreemptionValue(const OperationPreemptionDTO& value) {
  return CanonicalValue(CanonicalValue::Object{
      {"kind", CanonicalValue(std::string(to_string(value.kind)))},
      {"by", CanonicalValue(CanonicalValue::Object{
          {"operation_id", Number(value.by.operation_id.value(), "operation number")},
          {"node_instance_id", CanonicalValue(value.by.node_instance_id.value())},
          {"node_attempt_id", Number(value.by.node_attempt_id.value(), "node attempt number")}})}});
}

[[nodiscard]] CanonicalValue MotionTakeoverValue(const MotionTakeoverDecisionDTO& value) {
  Require(value.locally_valid(), "operation_journal_record_invalid",
          "motion takeover decision is invalid");
  return CanonicalValue(CanonicalValue::Object{
      {"incumbent_operation_id",
       Number(value.incumbent_operation_id.value(), "operation number")},
      {"path", CanonicalValue(std::string(to_string(value.path)))},
      {"cause", ErrorValue(value.cause)},
      {"decided_at_ns", Timestamp(value.decided_at_ns, "takeover decision timestamp")},
      {"elapsed_since_acceptance_ns",
       Timestamp(value.elapsed_since_acceptance_ns, "takeover elapsed time")},
      {"preparation_budget_ns",
       Timestamp(value.preparation_budget_ns, "takeover preparation budget")},
      {"planning_budget_ns", Timestamp(value.planning_budget_ns, "takeover planning budget")},
      {"stop_event_id", value.stop_event_id
                            ? Number(value.stop_event_id->value(), "Stop event")
                            : CanonicalValue(nullptr)}});
}

[[nodiscard]] CanonicalValue RequestContextValue(const PublicRequestContextDTO& context) {
  return CanonicalValue(CanonicalValue::Object{
      {"request_key", CanonicalValue(RequestKeyText(context.request_key))},
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

[[nodiscard]] bool IsTerminal(OperationStateKind state) noexcept {
  return state == OperationStateKind::kCompleted || state == OperationStateKind::kStopped ||
         state == OperationStateKind::kFailed || state == OperationStateKind::kInterrupted ||
         state == OperationStateKind::kInterventionRequired ||
         state == OperationStateKind::kCancelledPreDispatch;
}

[[nodiscard]] std::string_view PayloadKindName(OperationParticipantPayloadKind kind) noexcept {
  return kind == OperationParticipantPayloadKind::kMotion ? "motion" : "capability_command";
}

[[nodiscard]] std::string_view CoordinationName(OperationCoordinationPolicy policy) noexcept {
  return policy == OperationCoordinationPolicy::kIndependent ? "independent" : "coordinated_all";
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
  if (!reference.has_value()) return CanonicalValue(nullptr);
  std::string_view type;
  CanonicalValue id{nullptr};
  switch (reference->type) {
    case ReferenceDTO::Type::kComponentFrame: {
      type = "component_frame";
      const auto& frame = std::get<ProgramFrameReferenceDTO>(reference->id);
      id = CanonicalValue(CanonicalValue::Object{
          {"component_id", CanonicalValue(frame.component_id.value())},
          {"frame_id", CanonicalValue(frame.frame_id.value())}});
      break;
    }
    case ReferenceDTO::Type::kComponentBody: {
      type = "component_body";
      const auto& body = std::get<ProgramCollisionBodyReferenceDTO>(reference->id);
      id = CanonicalValue(CanonicalValue::Object{
          {"component_id", CanonicalValue(body.component_id.value())},
          {"collision_body_id", CanonicalValue(body.collision_body_id.value())}});
      break;
    }
    case ReferenceDTO::Type::kSceneResource: type = "scene_resource"; break;
    case ReferenceDTO::Type::kRuntimeObject: type = "runtime_object"; break;
    case ReferenceDTO::Type::kPose: type = "pose"; break;
    case ReferenceDTO::Type::kCellFrame: type = "cell_frame"; break;
  }
  if (reference->type == ReferenceDTO::Type::kSceneResource ||
      reference->type == ReferenceDTO::Type::kRuntimeObject)
    id = CanonicalValue(std::string(reference_identity_value(*reference)));
  return CanonicalValue(CanonicalValue::Object{
      {"id", std::move(id)},
      {"pose", reference->pose.has_value() ? PoseValue(*reference->pose) : CanonicalValue(nullptr)},
      {"type", CanonicalValue(std::string(type))}});
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
      {"program_run_id", RunValue(span->program_run_id)},
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
          "durable admission does not accept motion-interrupt scopes");
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
      {"base_frame", CanonicalValue(CanonicalValue::Object{
          {"component_id", CanonicalValue(motion.base_frame.component_id.value())},
          {"frame_id", CanonicalValue(motion.base_frame.frame_id.value())}})},
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
                       {"node_attempt_id",
                        Number(scope->node_attempt_id.value(), "node attempt number")},
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
        {"payload",
         std::holds_alternative<MotionParticipantIntentDTO>(participant.payload)
             ? MotionPayloadValue(std::get<MotionParticipantIntentDTO>(participant.payload))
             : CapabilityPayloadValue(std::get<CapabilityCommandIntentDTO>(participant.payload))},
        {"required_capability_id", CanonicalValue(participant.required_capability_id.value())},
        {"target", CanonicalValue(CanonicalValue::Object{
            {"component_id", CanonicalValue(participant.target.component_id.value())},
            {"command_target_id", CanonicalValue(participant.target.command_target_id.value())}})},
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
      {"program_run_id", RunValue(intent.program_run_id)},
      {"request_attribution", RequestAttributionValue(intent.attribution)},
      {"request_context", RequestContextValue(intent.request_context)},
  };
  if (intent.manual_handover) {
    const auto& context = *intent.manual_handover;
    result.emplace_back(
        "manual_handover",
        CanonicalValue(CanonicalValue::Object{
            {"claim_generation", Number(context.claim_generation.value(), "manual parent claim")},
            {"hold_revision", Number(context.hold_revision, "manual hold revision")},
            {"program_run_id", Number(context.program_run_id.value(), "manual parent run")}}));
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
        {"participant_id", OperationParticipantIdValue(assignment.participant_id)},
    });
  }
  return CanonicalValue(CanonicalValue::Object{
      {"accepted_at_ns", Timestamp(accepted.accepted_at_ns, "operation acceptance timestamp")},
      {"operation_id", Number(accepted.operation_id.value(), "operation number")},
      {"operation_revision", Number(accepted.operation_revision.value(), "operation revision")},
      {"participant_assignments", CanonicalValue(std::move(assignments))},
      {"replay_status", CanonicalValue(std::string(to_string(accepted.replay_status)))},
      {"request_key", CanonicalValue(RequestKeyText(accepted.request_key))},
  });
}

[[nodiscard]] CanonicalValue ParticipantStopValue(
    const std::optional<ParticipantStopObservationDTO>& stop) {
  if (!stop) return CanonicalValue(nullptr);
  return CanonicalValue(CanonicalValue::Object{
      {"attempt", Number(stop->attempt, "participant Stop attempt")},
      {"attribution_class", CanonicalValue(std::string(to_string(stop->attribution_class)))},
      {"containing_stage_id", OptionalIdentity(stop->containing_stage_id)},
      {"containing_stage_index", stop->containing_stage_index
                                     ? Number(*stop->containing_stage_index, "containing stage")
                                     : CanonicalValue(nullptr)},
      {"escalated", CanonicalValue(stop->escalated)},
      {"mode", CanonicalValue(std::string(to_string(stop->mode)))},
      {"phase", CanonicalValue(std::string(to_string(stop->phase)))},
      {"plan_digest", OptionalIdentity(stop->plan_digest)},
      {"stop_event_id", Number(stop->stop_event_id.value(), "participant Stop event")},
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
        {"command_id", ControllerGoalIdValue(participant.command_id)},
        {"participant_id", OperationParticipantIdValue(participant.participant_id)},
        {"payload_kind", CanonicalValue(std::string(PayloadKindName(participant.payload_kind)))},
        {"progress", CanonicalValue(participant.progress)},
        {"resources", StringArray(participant.resources)},
        {"state", CanonicalValue(std::string(OperationStateName(participant.state)))},
        {"stop", ParticipantStopValue(participant.stop)},
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
      {"coordination_policy",
       CanonicalValue(std::string(CoordinationName(state.coordination_policy)))},
      {"operation_id", Number(state.operation_id.value(), "operation number")},
      {"operation_revision", Number(state.operation_revision.value(), "operation revision")},
      {"participants", CanonicalValue(std::move(participants))},
      {"program_binding_id", CanonicalValue(state.program_binding_id.value())},
      {"program_run_id", RunValue(state.program_run_id)},
      {"runtime_scale_generation",
       state.runtime_scale_generation.has_value()
           ? Number(state.runtime_scale_generation->value(), "runtime scale generation")
           : CanonicalValue(nullptr)},
      {"state", CanonicalValue(std::string(OperationStateName(state.state)))},
      {"stop_event_id", state.stop_event_id
                            ? Number(state.stop_event_id->value(), "operation Stop event")
                            : CanonicalValue(nullptr)},
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
        {"participant_id", OperationParticipantIdValue(diagnostic.participant_id)},
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
      {"operation_id", Number(request.operation_id.value(), "operation number")},
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
      {"operation_id", Number(accepted.operation_id.value(), "operation number")},
      {"operation_revision_after_acceptance",
       Number(accepted.operation_revision_after_acceptance.value(),
              "operation revision after control")},
      {"replay_status", CanonicalValue(std::string(to_string(accepted.replay_status)))},
      {"request_key", CanonicalValue(RequestKeyText(accepted.request_key))},
  });
}

[[nodiscard]] CanonicalValue RuntimeScaleRequestValue(const RuntimeScaleRequestDTO& request) {
  return CanonicalValue(CanonicalValue::Object{
      {"expected_operation_revision",
       Number(request.expected_operation_revision.value(), "scale expected operation revision")},
      {"expected_runtime_scale_generation",
       Number(request.expected_runtime_scale_generation.value(),
              "expected runtime scale generation")},
      {"operation_id", Number(request.operation_id.value(), "operation number")},
      {"request_attribution", RequestAttributionValue(request.attribution)},
      {"request_context", RequestContextValue(request.request_context)},
      {"scale", CanonicalValue(request.scale)},
  });
}

[[nodiscard]] CanonicalValue RuntimeScaleAcceptedValue(const RuntimeScaleAcceptedDTO& accepted) {
  return CanonicalValue(CanonicalValue::Object{
      {"accepted_at_ns", Timestamp(accepted.accepted_at_ns, "scale acceptance timestamp")},
      {"operation_id", Number(accepted.operation_id.value(), "operation number")},
      {"operation_revision", Number(accepted.operation_revision.value(), "operation revision")},
      {"replay_status", CanonicalValue(std::string(to_string(accepted.replay_status)))},
      {"request_key", CanonicalValue(RequestKeyText(accepted.request_key))},
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
        {"controller_goal_id", ControllerGoalIdValue(participant.controller_goal_id)},
        {"controller_topology_id", CanonicalValue(participant.controller_topology_id.value())},
        {"coordinated_barrier_id", OptionalIdentity(participant.coordinated_barrier_id)},
        {"error", ErrorValue(participant.error)},
        {"participant_id", OperationParticipantIdValue(participant.participant_id)},
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
      {"operation_id", Number(status->operation_id.value(), "operation number")},
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
                                         const CanonicalValue& intent, const CanonicalValue& result,
                                         const OperationStateDTO& state,
                                         const RuntimeScaleStatusDTO* scale_status) {
  return CanonicalValue(CanonicalValue::Object{
      {"api_version", CanonicalValue("manyforge.operation_journal_record.v10")},
      {"intent", intent},
      {"operation_state", OperationStateValue(state)},
      {"record_kind", CanonicalValue(std::string(record_kind))},
      {"result", result},
      {"runtime_scale_status", RuntimeScaleStatusValue(scale_status)},
  });
}

[[nodiscard]] bool ValidState(const OperationStateDTO& state) noexcept {
  if (!state.operation_id.nonzero() || !state.operation_revision.nonzero() ||
      state.program_binding_id.empty() ||
      (state.program_run_id && !state.program_run_id->nonzero()) ||
      state.participants.empty() || state.participants.size() > kMaxOperationParticipants ||
      state.updated_at_ns < 0 ||
      state.journal_capture_order > static_cast<std::uint64_t>(INT64_MAX) ||
      (state.stop_event_id && !state.stop_event_id->nonzero())) {
    return false;
  }
  if (state.preemption) {
    const auto& fact = *state.preemption;
    if (to_string(fact.kind).empty() || !has_program_execution_scope(state) ||
        state.journal_capture_order == 0U || !fact.by.operation_id.nonzero() ||
        fact.by.operation_id == state.operation_id ||
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
    // The journal stores a participant by its ordinal in its operation and
    // its goal by generation (contract 1.5: the goal is its participant's).
    if (participant.participant_id.operation_id != state.operation_id ||
        participant.participant_id.ordinal == 0U ||
        participant.command_id.participant() != participant.participant_id ||
        participant.command_id.generation == 0U ||
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

[[nodiscard]] const std::set<std::string>& ExpectedSchemaObjects() {
  static const std::set<std::string> names{
      "index:journal_operation_sequence",
      "index:journal_stop_event_identity",
      "index:operation_snapshot_sequence",
      "table:journal_metadata",
      "table:journal_records",
      "table:operation_snapshots",
  };
  return names;
}

template <typename T>
[[nodiscard]] Result<T> AsResult(auto&& function) {
  try {
    return Result<T>::Ok(function());
  } catch (const std::exception&) {
    return Result<T>::Err(CaughtError());
  }
}

[[nodiscard]] std::uint64_t PositiveIdentifier(std::int64_t value, std::string_view label) {
  Require(value > 0 && static_cast<std::uint64_t>(value) <= kMaximumSafeInteger,
          "operation_journal_corrupt", std::string(label) + " is outside the valid identity range");
  return static_cast<std::uint64_t>(value);
}

/// Whether the snapshot now holds `state` (an older capture leaves it).
[[nodiscard]] bool StoreOperationSnapshot(sqlite3* database, const OperationStateDTO& state,
                                          std::uint64_t snapshot_sequence) {
  RequireState(state);
  Require(snapshot_sequence > 0U && snapshot_sequence <= kMaximumSafeInteger,
          "operation_journal_record_invalid", "operation journal sequence is invalid");
  Statement previous(database,
                     "SELECT journal_capture_order,operation_revision FROM operation_snapshots "
                     "WHERE operation_id=?");
  previous.Bind(1, state.operation_id);
  if (previous.Step()) {
    const auto order = previous.Integer(0);
    const auto revision = previous.Integer(1);
    Require(order >= 0 && !previous.Step(), "operation_journal_corrupt",
            "operation snapshot capture order is invalid");
    if (order > 0) {
      if (state.journal_capture_order < static_cast<std::uint64_t>(order)) return false;
      if (state.journal_capture_order == static_cast<std::uint64_t>(order)) return false;
      Require(state.operation_revision.value() >= static_cast<std::uint64_t>(revision),
              "operation_journal_corrupt", "newer capture regresses the public operation revision");
    }
  }
  Statement upsert(database,
                   "INSERT INTO operation_snapshots("
                   "operation_id,operation_revision,program_binding_id,"
                   "program_run_id,coordination_policy,state,updated_at_ns,stop_event_id,"
                   "runtime_scale_generation,"
                   "snapshot_sequence,journal_capture_order) VALUES(?,?,?,?,?,?,?,?,?,?,?) "
                   "ON CONFLICT(operation_id) DO UPDATE SET "
                   "operation_revision=excluded.operation_revision,"
                   "program_binding_id=excluded.program_binding_id,"
                   "program_run_id=excluded.program_run_id,"
                   "coordination_policy=excluded.coordination_policy,state=excluded.state,"
                   "updated_at_ns=excluded.updated_at_ns,stop_event_id=excluded.stop_event_id,"
                   "runtime_scale_generation=excluded.runtime_scale_generation,"
                   "snapshot_sequence=excluded.snapshot_sequence,"
                   "journal_capture_order=excluded.journal_capture_order "
                   "WHERE excluded.operation_revision>=operation_snapshots.operation_revision");
  upsert.Bind(1, state.operation_id);
  upsert.Bind(2, static_cast<std::int64_t>(state.operation_revision.value()));
  upsert.Bind(3, state.program_binding_id.value());
  upsert.Bind(4, state.program_run_id);
  upsert.Bind(5, CoordinationName(state.coordination_policy));
  upsert.Bind(6, OperationStateName(state.state));
  upsert.Bind(7, state.updated_at_ns);
  if (state.stop_event_id.has_value()) {
    upsert.Bind(8, static_cast<std::int64_t>(state.stop_event_id->value()));
  } else {
    upsert.BindNull(8);
  }
  if (state.runtime_scale_generation.has_value()) {
    upsert.Bind(9, static_cast<std::int64_t>(state.runtime_scale_generation->value()));
  } else {
    upsert.BindNull(9);
  }
  upsert.Bind(10, static_cast<std::int64_t>(snapshot_sequence));
  upsert.Bind(11, static_cast<std::int64_t>(state.journal_capture_order));
  Require(!upsert.Step(), "operation_journal_corrupt",
          "operation snapshot upsert returned an unexpected row");
  return sqlite3_changes(database) == 1;
}

/// An owner transaction number, or null for a role that has none.
[[nodiscard]] CanonicalValue OwnerTransactionValue(
    const std::optional<CellAdmissionOwnerTransactionId>& value) {
  return value ? Number(CellAdmissionOwnerTransactionNumber(*value), "owner transaction id")
               : CanonicalValue(nullptr);
}

/// `cell_claim_acquired` payload: the claim's fields, its scope and its
/// evidence, under the names of the former claim columns.
[[nodiscard]] CanonicalValue CellClaimAcquiredValue(const CellAdmissionClaimDTO& claim) {
  Require(ValidateCellAdmissionClaimIntent(claim.intent).ok() && claim.claim_generation.nonzero() &&
              claim.acquired_admission_generation.nonzero() && claim.accepted_at_ns >= 0,
          "operation_journal_record_invalid", "cell-admission acquire record is invalid");
  const auto& intent = claim.intent;
  CanonicalValue::Array configurations;
  configurations.reserve(intent.scope.configurations.size());
  for (const auto& configuration : intent.scope.configurations) {
    configurations.emplace_back(CanonicalValue::Object{
        {"active_configuration_generation",
         Number(configuration.active_configuration_generation.value(),
                "active configuration generation")},
        {"active_configuration_id", CanonicalValue(configuration.active_configuration_id.value())},
        {"control_domain_id", CanonicalValue(configuration.control_domain_id.value())}});
  }
  const auto* cycle = intent.program_cycle_evidence ? &*intent.program_cycle_evidence : nullptr;
  const auto* offer = intent.intervention_evidence ? &*intent.intervention_evidence : nullptr;
  const auto* node = intent.program_node_evidence ? &*intent.program_node_evidence : nullptr;
  CanonicalValue parent(nullptr);
  if (node != nullptr) {
    parent = Number(node->parent_program_cycle_claim_generation.value(), "parent claim generation");
  } else if (offer != nullptr && offer->parent_program_cycle_claim_generation) {
    parent = Number(offer->parent_program_cycle_claim_generation->value(),
                    "parent claim generation");
  }
  return CanonicalValue(CanonicalValue::Object{
      {"accepted_at_ns", Timestamp(claim.accepted_at_ns, "claim acceptance timestamp")},
      {"acquired_admission_generation",
       Number(claim.acquired_admission_generation.value(), "acquired admission generation")},
      {"applied_program_revision",
       cycle ? CanonicalValue(cycle->applied_program_revision.value()) : CanonicalValue(nullptr)},
      {"claim_generation", Number(claim.claim_generation.value(), "claim generation")},
      {"claim_kind", CanonicalValue(std::string(to_string(intent.claim_kind)))},
      {"configurations", CanonicalValue(std::move(configurations))},
      {"draft_program_revision",
       cycle ? CanonicalValue(cycle->draft_program_revision.value()) : CanonicalValue(nullptr)},
      {"expected_admission_generation",
       Number(intent.expected_admission_generation.value(), "expected admission generation")},
      {"offer_digest", offer ? CanonicalValue(offer->offer_digest.value()) : CanonicalValue(nullptr)},
      {"offer_transaction_id",
       offer ? OwnerTransactionValue(offer->offer_transaction_id) : CanonicalValue(nullptr)},
      {"offering_owner_role",
       offer ? CanonicalValue(std::string(ApplicationRoleText(offer->offering_owner_role)))
             : CanonicalValue(nullptr)},
      {"owner_role", CanonicalValue(std::string(ApplicationRoleText(intent.owner_role)))},
      {"owner_transaction_id", OwnerTransactionValue(intent.owner_transaction_id)},
      {"parent_program_cycle_claim_generation", std::move(parent)},
      {"program_node_attempt_id",
       node ? Number(node->node_attempt_id.value(), "node attempt number") : CanonicalValue(nullptr)},
      {"program_node_instance_id",
       node ? CanonicalValue(node->node_instance_id) : CanonicalValue(nullptr)},
      {"program_node_owner_api", node ? CanonicalValue(node->owner_api) : CanonicalValue(nullptr)},
      {"program_node_run_id",
       node ? Number(node->program_run_id.value(), "Program node run") : CanonicalValue(nullptr)},
      {"request_key", CanonicalValue(RequestKeyText(intent.request_key))},
      {"resources", StringArray(intent.scope.resources)}});
}

/// `cell_claim_released` payload: the claim's generation and its release.
[[nodiscard]] CanonicalValue CellClaimReleasedValue(const JournalCellClaimReleased& released) {
  const auto& claim = released.claim;
  const auto& release = released.release;
  Require(ValidateCellAdmissionClaimRelease(release).ok() &&
              claim.claim_generation == release.claim_generation &&
              claim.intent.owner_role == release.owner_role &&
              claim.intent.owner_transaction_id == release.owner_transaction_id &&
              claim.acquired_admission_generation == release.acquired_admission_generation,
          "operation_journal_record_invalid", "cell-admission release record is invalid");
  return CanonicalValue(CanonicalValue::Object{
      {"claim_generation", Number(release.claim_generation.value(), "claim generation")},
      {"release_acquired_admission_generation",
       Number(release.acquired_admission_generation.value(), "release admission generation")},
      {"release_evidence_digest", CanonicalValue(release.settlement.evidence_digest.value())},
      {"release_owner_role", CanonicalValue(std::string(ApplicationRoleText(release.owner_role)))},
      {"release_owner_transaction_id", OwnerTransactionValue(release.owner_transaction_id)},
      {"release_request_key", CanonicalValue(RequestKeyText(release.request_key))},
      {"release_settlement_kind",
       CanonicalValue(std::string(to_string(release.settlement.settlement_kind)))},
      {"released_at_ns", Timestamp(released.released_at_ns, "claim release timestamp")}});
}

}  // namespace

class SqliteOperationJournal::Impl final {
 public:
  Impl(std::filesystem::path database_path, CellId cell_id, std::size_t history_capacity,
       std::function<std::string(std::int64_t)> wall_time_text,
       std::function<void(std::string_view)> critical_section_observer)
      : database_path_(ValidatePath(std::move(database_path))),
        cell_id_(std::move(cell_id)),
        history_capacity_(history_capacity),
        wall_time_text_(std::move(wall_time_text)),
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
    std::lock_guard lock(write_mutex_);
    if (closed_) {
      return;
    }
    MarkRecordsLost();
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

  void BeginStart(ApplicationStartId application_start_id, std::int64_t started_at_ns,
                       std::size_t request_capacity);

  JournalBatchResult WriteBatch(std::span<const JournalRecord> records,
                                std::uint64_t records_dropped);

  /// The operational rows only (owner lifecycle rows are not read back).
  OperationJournalEventPageDTO ReadEvents(std::uint64_t after_sequence, std::size_t limit);

 private:
  void RequireOpen() const {
    Require(!closed_ && database_ != nullptr, "operation_journal_closed",
            "operation journal database is closed");
  }

  // Event reads use their own read-only WAL connection and lock. They observe
  // a committed snapshot and never take the write lock; SQLite WAL readers
  // never block the writer.
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
    Exec(database_, "PRAGMA synchronous=NORMAL");
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
                         "cell_id,history_capacity) VALUES(1,?,?,?,?)");
      metadata.Bind(1, kOperationJournalSchemaId);
      metadata.Bind(2, kOperationJournalSchemaVersion);
      metadata.Bind(3, cell_id_.value());
      metadata.Bind(4, static_cast<std::int64_t>(history_capacity_));
      Require(!metadata.Step(), "operation_journal_corrupt",
              "operation journal metadata insert returned an unexpected row");
    }
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
                       "SELECT schema_id,schema_version,cell_id,history_capacity "
                       "FROM journal_metadata WHERE singleton=1");
    Require(metadata.Step() && metadata.Text(0) == kOperationJournalSchemaId &&
                metadata.Integer(1) == kOperationJournalSchemaVersion &&
                metadata.Text(2) == cell_id_.value() &&
                metadata.Integer(3) == static_cast<std::int64_t>(history_capacity_) &&
                !metadata.Step(),
            "operation_journal_storage_foreign",
            "operation journal metadata is invalid or configuration changed");
    transaction.Commit();
  }

  // Inside the current transaction (write_mutex_ held). Each returns the
  // row's sequence.
  void WriteTransaction(std::span<const JournalRecord> records, std::size_t first,
                        std::size_t end, JournalBatchResult& result);
  void WriteRecord(const JournalRecord& record);
  std::uint64_t WriteOperationAccepted(const OperationIntentDTO& intent,
                                       const OperationAcceptedDTO& accepted,
                                       const OperationStateDTO& state);
  std::uint64_t WriteControlAccepted(const OperationControlRequestDTO& request,
                                     const OperationControlAcceptedDTO& accepted,
                                     const OperationStateDTO& state);
  std::uint64_t WriteRuntimeScaleAccepted(const RuntimeScaleRequestDTO& request,
                                          const RuntimeScaleAcceptedDTO& accepted,
                                          const OperationStateDTO& state,
                                          const RuntimeScaleStatusDTO& scale_status);
  std::uint64_t WriteOperationState(
      const OperationStateDTO& state, const RuntimeScaleStatusDTO* scale_status,
      const char* transition_reason,
      std::span<const PreparedMotionDiagnosticsDTO> prepared_motion_diagnostics);
  std::uint64_t WriteInterruptRuleStateChanged(const IoRuleStateChangeDTO& change);
  std::uint64_t WriteActiveTcpChanged(const ActiveTcpChangeDTO& change);
  std::uint64_t WriteCellClaimAcquired(const CellAdmissionClaimDTO& claim);
  std::uint64_t WriteCellClaimReleased(const JournalCellClaimReleased& released);

  std::uint64_t Append(std::string_view record_kind,
                       const std::optional<RequestKey>& request_key,
                       const CanonicalValue& record_value, const OperationStateDTO& state,
                       std::int64_t occurred_at_ns, bool compact_operations = false);
  std::uint64_t AppendStopEvent(std::string_view record_kind, StopEventId event_id,
                                std::int64_t occurred_at_ns, const std::string& payload_json);
  /// An operation-less, Stop-less row (rule, Active TCP and claim rows).
  std::uint64_t AppendEvent(std::string_view record_kind, std::int64_t occurred_at_ns,
                            const std::string& payload_json);
  void CompactFinishedStopEvents();
  void CompactReleasedClaims();
  void TrackOperationState(const OperationStateDTO& state);
  void CompactOperations();

  /// Sets `journal_metadata.records_lost` once records were lost in this
  /// start (write_mutex_ held). If that write fails too, the next batch or
  /// the close tries again; nothing more is done.
  void MarkRecordsLost() noexcept {
    if (!records_lost_ || records_lost_marked_ || closed_) return;
    try {
      Transaction transaction(database_);
      Exec(database_, "UPDATE journal_metadata SET records_lost=1 WHERE singleton=1");
      transaction.Commit();
      records_lost_marked_ = true;
    } catch (...) {
    }
  }

  /// The wall time of the action at `occurred_at_ns` (steady clock), from the
  /// system/steady offset sampled once for the batch or direct write.
  [[nodiscard]] std::string wall_time_for(std::int64_t occurred_at_ns) const {
    auto text = wall_time_text_ ? wall_time_text_(occurred_at_ns)
                                : WallTimeText(occurred_at_ns + system_minus_steady_ns_);
    ValidateWallTimeText(text);
    return text;
  }

  std::filesystem::path database_path_;
  CellId cell_id_;
  std::size_t history_capacity_;
  // Set once by BeginStart, before any write.
  std::size_t request_capacity_{};
  std::function<std::string(std::int64_t)> wall_time_text_;
  std::function<void(std::string_view)> critical_section_observer_;
  // Leaf lock: held across one transaction only. The journal writer thread
  // (WriteBatch) and the remaining direct writers take it.
  std::mutex write_mutex_;
  std::int64_t system_minus_steady_ns_{};
  // Writer memory for removing old finished operations (guarded by
  // write_mutex_): every tracked operation, true once its written state is
  // terminal, and the finished ones in finish order.
  std::map<OperationNumber, bool> operation_finished_;
  std::deque<OperationNumber> finished_operations_;
  // A record was dropped before the journal or lost by a failed write.
  bool records_lost_{};
  bool records_lost_marked_{};
  sqlite3* database_{};
  bool closed_{};
  std::mutex reader_mutex_;
  sqlite3* reader_{};
};

std::uint64_t SqliteOperationJournal::Impl::Append(
    std::string_view record_kind,
    const std::optional<RequestKey>& request_key, const CanonicalValue& record_value,
    const OperationStateDTO& state, std::int64_t occurred_at_ns, const bool compact_operations) {
  Require(!record_kind.empty() && occurred_at_ns >= 0,
          "operation_journal_record_invalid", "operation journal record identity is invalid");
  RequireState(state);
  const auto payload_json = content_identity::canonical_json(record_value);
  Require(payload_json.size() <= kMaximumRecordJsonBytes, "operation_journal_record_too_large",
          "operation journal record exceeds the fixed payload bound");
  const auto payload_digest = content_identity::sha256_digest(payload_json);
  const auto occurred_at_wall = wall_time_for(occurred_at_ns);
  Require(request_capacity_ > 0U, "operation_journal_corrupt",
          "operation journal owner epoch was not begun");

  Statement insert(database_,
                   "INSERT INTO journal_records(record_kind,request_key,"
                   "operation_id,operation_revision,occurred_at_ns,occurred_at_wall,"
                   "payload_json,payload_digest) VALUES(?,?,?,?,?,?,?,?)");
  insert.Bind(1, record_kind);
  if (request_key.has_value()) {
    insert.Bind(2, *request_key);
  } else {
    insert.BindNull(2);
  }
  insert.Bind(3, state.operation_id);
  insert.Bind(4, static_cast<std::int64_t>(state.operation_revision.value()));
  insert.Bind(5, occurred_at_ns);
  insert.Bind(6, occurred_at_wall);
  insert.Bind(7, payload_json);
  insert.Bind(8, payload_digest);
  Require(!insert.Step(), "operation_journal_corrupt",
          "operation journal insert returned an unexpected row");
  const auto sequence = sqlite3_last_insert_rowid(database_);
  const auto bounded_sequence = PositiveIdentifier(sequence, "operation journal sequence");
  const bool stored = StoreOperationSnapshot(database_, state, bounded_sequence);

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
  coalesce.Bind(1, state.operation_id);
  coalesce.Bind(2, static_cast<std::int64_t>(sequence));
  Require(!coalesce.Step(), "operation_journal_corrupt",
          "operation journal current-state coalescing returned an unexpected row");
  if (stored) TrackOperationState(state);
  if (compact_operations) CompactOperations();
  return bounded_sequence;
}

// The writer's memory follows each operation's snapshot state: an operation
// is finished while its snapshot state is terminal (IsTerminal, the same set
// as the Kernel's and the launcher report's).
void SqliteOperationJournal::Impl::TrackOperationState(const OperationStateDTO& state) {
  const bool finished = IsTerminal(state.state);
  const auto [entry, added] = operation_finished_.try_emplace(state.operation_id, finished);
  if (!added && entry->second == finished) return;
  entry->second = finished;
  if (finished) {
    finished_operations_.push_back(state.operation_id);
  } else if (!added) {
    finished_operations_.erase(
        std::find(finished_operations_.begin(), finished_operations_.end(), state.operation_id));
  }
}

// Old finished operations go (snapshot and every row) beyond the retained
// bound less the unfinished ones; an unfinished operation stays, unless
// records were lost: then unfinished ones are also bounded, oldest first.
void SqliteOperationJournal::Impl::CompactOperations() {
  const auto retained = std::min(history_capacity_, request_capacity_);
  auto unfinished = operation_finished_.size() - finished_operations_.size();
  std::vector<OperationNumber> removed;
  if (records_lost_) {
    for (const auto& [operation_id, finished] : operation_finished_) {
      if (unfinished <= retained) break;
      if (finished) continue;
      removed.push_back(operation_id);
      --unfinished;
    }
  }
  const auto finished_budget = unfinished >= retained ? 0U : retained - unfinished;
  const auto finished_removed = finished_operations_.size() > finished_budget
                                    ? finished_operations_.size() - finished_budget
                                    : 0U;
  removed.insert(removed.end(), finished_operations_.begin(),
                 finished_operations_.begin() + static_cast<std::ptrdiff_t>(finished_removed));
  for (const auto& operation_id : removed) {
    Statement remove_snapshot(database_, "DELETE FROM operation_snapshots WHERE operation_id=?");
    remove_snapshot.Bind(1, operation_id);
    Require(!remove_snapshot.Step(), "operation_journal_corrupt",
            "resolved operation snapshot compaction failed");
    Statement remove_records(database_, "DELETE FROM journal_records WHERE operation_id=?");
    remove_records.Bind(1, operation_id);
    Require(!remove_records.Step(), "operation_journal_corrupt",
            "resolved operation record compaction failed");
  }
  finished_operations_.erase(
      finished_operations_.begin(),
      finished_operations_.begin() + static_cast<std::ptrdiff_t>(finished_removed));
  for (const auto& operation_id : removed) operation_finished_.erase(operation_id);
}

std::uint64_t SqliteOperationJournal::Impl::AppendStopEvent(
    const std::string_view record_kind, const StopEventId event_id,
    const std::int64_t occurred_at_ns, const std::string& payload_json) {
  Require(payload_json.size() <= kMaximumRecordJsonBytes, "operation_journal_record_too_large",
          "Stop event row exceeds the fixed payload bound");
  const auto payload_digest = content_identity::sha256_digest(payload_json);
  const auto occurred_at_wall = wall_time_for(occurred_at_ns);
  // One row of each kind per event: the Kernel hands each row once and never
  // retries.
  Statement insert(database_,
                   "INSERT INTO journal_records(record_kind,request_key,stop_event_id,"
                   "operation_id,operation_revision,occurred_at_ns,occurred_at_wall,"
                   "payload_json,payload_digest) VALUES(?,NULL,?,NULL,NULL,?,?,?,?)");
  insert.Bind(1, record_kind);
  insert.Bind(2, static_cast<std::int64_t>(event_id.value()));
  insert.Bind(3, occurred_at_ns);
  insert.Bind(4, occurred_at_wall);
  insert.Bind(5, payload_json);
  insert.Bind(6, payload_digest);
  Require(!insert.Step(), "operation_journal_corrupt",
          "Stop event journal insert returned an unexpected row");
  const auto sequence =
      PositiveIdentifier(sqlite3_last_insert_rowid(database_), "operation journal sequence");
  CompactFinishedStopEvents();
  return sequence;
}

std::uint64_t SqliteOperationJournal::Impl::AppendEvent(const std::string_view record_kind,
                                                        const std::int64_t occurred_at_ns,
                                                        const std::string& payload_json) {
  const auto payload_digest = content_identity::sha256_digest(payload_json);
  const auto occurred_at_wall = wall_time_for(occurred_at_ns);
  Statement insert(database_,
                   "INSERT INTO journal_records(record_kind,request_key,operation_id,"
                   "operation_revision,occurred_at_ns,occurred_at_wall,payload_json,"
                   "payload_digest) VALUES(?,NULL,NULL,NULL,?,?,?,?)");
  insert.Bind(1, record_kind);
  insert.Bind(2, occurred_at_ns);
  insert.Bind(3, occurred_at_wall);
  insert.Bind(4, payload_json);
  insert.Bind(5, payload_digest);
  Require(!insert.Step(), "operation_journal_corrupt",
          std::string(record_kind) + " insert returned a row");
  return PositiveIdentifier(sqlite3_last_insert_rowid(database_), "operation journal sequence");
}

// Stop-row retention (build spec section 6, "Kernel journal Stop rows"):
// both rows of every finished event older than the newest
// kMaximumRetainedFinishedStopEvents finished ones are deleted; see that
// constant for why this is safe and bounded. Stop rows anchor no operation
// snapshot, so nothing else refers to them.
void SqliteOperationJournal::Impl::CompactFinishedStopEvents() {
  Statement compact(database_,
                    "DELETE FROM journal_records WHERE stop_event_id IN ("
                    "SELECT stop_event_id FROM journal_records "
                    "WHERE stop_event_id IS NOT NULL AND (record_kind='stop_event_completed' "
                    "OR stop_event_id<=(SELECT MAX(stop_event_id) FROM journal_records)-?) "
                    "GROUP BY stop_event_id ORDER BY stop_event_id DESC LIMIT -1 OFFSET ?)");
  compact.Bind(1, static_cast<std::int64_t>(request_capacity_));
  compact.Bind(2, static_cast<std::int64_t>(kMaximumRetainedFinishedStopEvents));
  Require(!compact.Step(), "operation_journal_corrupt",
          "finished Stop event compaction returned a row");
}

OperationJournalEventPageDTO SqliteOperationJournal::Impl::ReadEvents(
    const std::uint64_t after_sequence,
    const std::size_t limit) {
  Require(after_sequence <= kMaximumSafeInteger && limit > 0U &&
              limit <= kMaximumOperationJournalEventPage,
          "operation_journal_event_query_invalid",
          "operation journal event query exceeds its identity or page bound");

  std::lock_guard lock(reader_mutex_);
  Require(reader_ != nullptr, "operation_journal_closed", "operation journal database is closed");
  // One committed snapshot covers the page. SQLite opens it at the first
  // row read, so the observer (tests) is called there.
  ReadSnapshot snapshot(reader_);
  bool inside_snapshot = false;

  Statement query(reader_,
                  "SELECT sequence,record_kind,request_key,operation_id,"
                  "operation_revision,occurred_at_ns,occurred_at_wall,"
                  "payload_json,payload_digest FROM journal_records "
                  "WHERE sequence>? AND record_kind IN "
                  "('operation_accepted','control_accepted','stop_event_accepted',"
                  "'stop_event_completed',"
                  "'runtime_scale_accepted','operation_prepared',"
                  "'state_observed','interrupt_rule_state_changed',"
                  "'active_tcp_changed') "
                  "ORDER BY sequence LIMIT ?");
  query.Bind(1, static_cast<std::int64_t>(after_sequence));
  query.Bind(2, static_cast<std::int64_t>(limit + 1U));
  std::vector<OperationJournalEventDTO> events;
  events.reserve(limit + 1U);
  while (query.Step()) {
    if (!inside_snapshot) {
      inside_snapshot = true;
      if (critical_section_observer_) critical_section_observer_("read_events");
    }
    const auto sequence = PositiveIdentifier(query.Integer(0), "operation journal event sequence");
    const auto record_kind = query.Text(1);
    const auto request_key = query.IsNull(2) ? std::optional<RequestKey>{}
                                            : std::optional<RequestKey>(query.Key(2));
    const bool has_operation_id = !query.IsNull(3);
    const bool has_operation_revision = !query.IsNull(4);
    Require(has_operation_id == has_operation_revision, "operation_journal_corrupt",
            "operation journal event has an incomplete operation identity");
    const auto operation_id =
        has_operation_id ? std::optional<OperationNumber>(OperationNumber(
                               PositiveIdentifier(query.Integer(3), "operation number")))
                         : std::nullopt;
    const auto operation_revision =
        has_operation_revision ? std::optional<OperationRevision>(OperationRevision(
                                     PositiveIdentifier(query.Integer(4), "operation revision")))
                               : std::nullopt;
    const auto occurred_at_ns = query.Integer(5);
    const auto occurred_at_wall = query.Text(6);
    const auto payload_json = query.Text(7);
    const auto payload_digest = query.Text(8);
    Require(sequence > after_sequence && !record_kind.empty() &&
                occurred_at_ns >= 0 &&
                payload_json.size() <= kMaximumRecordJsonBytes &&
                content_identity::sha256_digest(payload_json) == payload_digest,
            "operation_journal_corrupt", "operation journal event record is invalid or corrupt");
    ValidateWallTimeText(occurred_at_wall);
    events.push_back(OperationJournalEventDTO{
        sequence, std::move(record_kind), request_key, operation_id, operation_revision,
        occurred_at_ns, std::move(occurred_at_wall), std::move(payload_json),
        std::move(payload_digest)});
  }
  const bool has_more = events.size() > limit;
  if (has_more) {
    events.erase(events.begin() + static_cast<std::ptrdiff_t>(limit), events.end());
  }
  const auto next_after_sequence = events.empty() ? after_sequence : events.back().sequence;
  return OperationJournalEventPageDTO{std::move(events), next_after_sequence, has_more};
}

void SqliteOperationJournal::Impl::BeginStart(const ApplicationStartId application_start_id,
                                              std::int64_t started_at_ns,
                                              std::size_t request_capacity) {
  Require(application_start_id.value != 0U && started_at_ns >= 0 && request_capacity > 0U &&
              request_capacity <= kMaximumRequestCapacity,
          "operation_journal_start_invalid",
          "operation journal start admission is invalid");
  std::lock_guard lock(write_mutex_);
  RequireOpen();
  Transaction transaction(database_);
  // `A` is stored once, in the metadata (build spec 6a); every start opens a
  // fresh journal (build spec 4.2).
  const auto start_text = FormatApplicationStartId(application_start_id);
  Statement record_start(database_,
                         "UPDATE journal_metadata SET application_start_id=?,started_at_ns=?,"
                         "request_capacity=? WHERE singleton=1");
  record_start.Bind(1, std::string_view(start_text.data(), start_text.size()));
  record_start.Bind(2, started_at_ns);
  record_start.Bind(3, static_cast<std::int64_t>(request_capacity));
  Require(!record_start.Step(), "operation_journal_corrupt",
          "operation journal start update returned an unexpected row");
  request_capacity_ = request_capacity;
  transaction.Commit();
}

JournalBatchResult SqliteOperationJournal::Impl::WriteBatch(
    const std::span<const JournalRecord> records, const std::uint64_t records_dropped) {
  JournalBatchResult result;
  // One offset for the whole batch: rows of one action carry one wall time.
  const auto offset = SystemMinusSteadyNanoseconds();
  // The direct writers wait for at most one transaction, never the batch.
  for (std::size_t first = 0U; first < records.size(); first += kMaximumTransactionRecords) {
    std::lock_guard lock(write_mutex_);
    if (records_dropped > 0U) records_lost_ = true;
    system_minus_steady_ns_ = offset;
    WriteTransaction(records, first, std::min(records.size(), first + kMaximumTransactionRecords),
                     result);
    if (!result.failures.empty()) records_lost_ = true;
    MarkRecordsLost();
  }
  if (records.empty() && records_dropped > 0U) {
    std::lock_guard lock(write_mutex_);
    records_lost_ = true;
    MarkRecordsLost();
  }
  return result;
}

// Records are inserted in span order, so the journal sequence follows it. A
// record that fails is rolled back alone; a transaction that fails as a whole
// (begin, commit, or SQLite ending it) reports each of its records.
void SqliteOperationJournal::Impl::WriteTransaction(const std::span<const JournalRecord> records,
                                                    const std::size_t first,
                                                    const std::size_t end,
                                                    JournalBatchResult& result) {
  const auto failures_before = result.failures.size();
  try {
    RequireOpen();
    Transaction transaction(database_);
    for (std::size_t index = first; index < end; ++index) {
      if (records[index].skipped) continue;
      try {
        Savepoint savepoint(database_);
        WriteRecord(records[index]);
        savepoint.Release();
      } catch (const std::exception&) {
        if (sqlite3_get_autocommit(database_) != 0) throw;
        result.failures.push_back({index, CaughtError()});
      }
    }
    transaction.Commit();
  } catch (const std::exception&) {
    const auto error = CaughtError();
    result.failures.resize(failures_before);
    for (std::size_t index = first; index < end; ++index)
      if (!records[index].skipped) result.failures.push_back({index, error});
  }
}

void SqliteOperationJournal::Impl::WriteRecord(const JournalRecord& record) {
  std::visit(
      [this](const auto& payload) {
        using Payload = std::decay_t<decltype(payload)>;
        if constexpr (std::is_same_v<Payload, JournalOperationAccepted>) {
          WriteOperationAccepted(payload.intent, payload.accepted, payload.state);
        } else if constexpr (std::is_same_v<Payload, JournalControlAccepted>) {
          WriteControlAccepted(payload.request, payload.accepted, payload.state);
        } else if constexpr (std::is_same_v<Payload, JournalOperationState>) {
          WriteOperationState(payload.state,
                              payload.scale_status ? &*payload.scale_status : nullptr,
                              payload.transition_reason, payload.prepared_motion_diagnostics);
        } else if constexpr (std::is_same_v<Payload, JournalRuntimeScaleAccepted>) {
          WriteRuntimeScaleAccepted(payload.request, payload.accepted, payload.state,
                                    payload.scale_status);
        } else if constexpr (std::is_same_v<Payload, JournalStopEventRow>) {
          const auto& stop = *payload.record;
          if (payload.completed) {
            AppendStopEvent(kStopEventCompletedRowKind, stop.event_id, stop.completed_at_ns,
                            EncodeStopEventCompleted(stop));
          } else {
            AppendStopEvent(kStopEventAcceptedRowKind, stop.event_id, stop.accepted_at_ns,
                            EncodeStopEventAccepted(stop));
          }
        } else if constexpr (std::is_same_v<Payload, IoRuleStateChangeDTO>) {
          WriteInterruptRuleStateChanged(payload);
        } else if constexpr (std::is_same_v<Payload, JournalCellClaimAcquired>) {
          WriteCellClaimAcquired(payload.claim);
        } else if constexpr (std::is_same_v<Payload, ActiveTcpChangeDTO>) {
          WriteActiveTcpChanged(payload);
        } else {
          static_assert(std::is_same_v<Payload, JournalCellClaimReleased>);
          WriteCellClaimReleased(payload);
        }
      },
      record.payload);
}

std::uint64_t SqliteOperationJournal::Impl::WriteOperationAccepted(
    const OperationIntentDTO& intent,
    const OperationAcceptedDTO& accepted, const OperationStateDTO& state) {
  Require(intent.request_context.request_key == accepted.request_key &&
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
  return Append("operation_accepted", intent.request_context.request_key,
                RecordValue("operation_accepted", OperationIntentValue(intent),
                            OperationAcceptedValue(accepted), state, nullptr),
                state, accepted.accepted_at_ns, true);
}

std::uint64_t SqliteOperationJournal::Impl::WriteControlAccepted(
    const OperationControlRequestDTO& request,
    const OperationControlAcceptedDTO& accepted, const OperationStateDTO& state) {
  Require(request.request_context.request_key == accepted.request_key &&
              request.operation_id == accepted.operation_id &&
              request.operation_id == state.operation_id &&
              accepted.operation_revision_after_acceptance == state.operation_revision &&
              accepted.replay_status == RequestReplayStatusDTO::kAcceptedNew &&
              accepted.accepted_at_ns == state.updated_at_ns,
          "operation_journal_record_invalid",
          "operation control acceptance does not match its durable request or state");
  return Append("control_accepted", request.request_context.request_key,
                RecordValue("control_accepted", ControlRequestValue(request),
                            ControlAcceptedValue(accepted), state, nullptr),
                state, accepted.accepted_at_ns);
}

std::uint64_t SqliteOperationJournal::Impl::WriteRuntimeScaleAccepted(
    const RuntimeScaleRequestDTO& request,
    const RuntimeScaleAcceptedDTO& accepted, const OperationStateDTO& state,
    const RuntimeScaleStatusDTO& scale_status) {
  Require(request.request_context.request_key == accepted.request_key &&
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
  return Append("runtime_scale_accepted", request.request_context.request_key,
      RecordValue("runtime_scale_accepted", RuntimeScaleRequestValue(request),
                  RuntimeScaleAcceptedValue(accepted), state, &scale_status),
      state, accepted.accepted_at_ns);
}

std::uint64_t SqliteOperationJournal::Impl::WriteOperationState(
    const OperationStateDTO& state,
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
  return Append(record_kind, std::nullopt,
                RecordValue(record_kind, CanonicalValue(std::move(intent)),
                            CanonicalValue(nullptr), state, scale_status),
                state, state.updated_at_ns);
}

std::uint64_t SqliteOperationJournal::Impl::WriteCellClaimAcquired(
    const CellAdmissionClaimDTO& claim) {
  const auto payload = content_identity::canonical_json(CellClaimAcquiredValue(claim));
  Require(payload.size() <= kMaximumRecordJsonBytes, "operation_journal_record_too_large",
          "cell-admission acquire record exceeds the fixed payload bound");
  return AppendEvent("cell_claim_acquired", claim.accepted_at_ns, payload);
}

std::uint64_t SqliteOperationJournal::Impl::WriteCellClaimReleased(
    const JournalCellClaimReleased& released) {
  const auto payload = content_identity::canonical_json(CellClaimReleasedValue(released));
  Require(payload.size() <= kMaximumRecordJsonBytes, "operation_journal_record_too_large",
          "cell-admission release record exceeds the fixed payload bound");
  const auto sequence = AppendEvent("cell_claim_released", released.released_at_ns, payload);
  CompactReleasedClaims();
  return sequence;
}

// Claim-row retention, as for the Stop rows: both rows of every claim
// released before the newest kMaximumRetainedReleasedClaims releases are
// deleted, keyed by the payload's claim_generation. Only generations of
// released rows are selected, so a claim not yet released keeps its row.
void SqliteOperationJournal::Impl::CompactReleasedClaims() {
  Statement compact(database_,
                    "DELETE FROM journal_records "
                    "WHERE record_kind IN ('cell_claim_acquired','cell_claim_released') "
                    "AND json_extract(payload_json,'$.claim_generation') IN ("
                    "SELECT json_extract(payload_json,'$.claim_generation') FROM journal_records "
                    "WHERE record_kind='cell_claim_released' "
                    "ORDER BY sequence DESC LIMIT -1 OFFSET ?)");
  compact.Bind(1, static_cast<std::int64_t>(kMaximumRetainedReleasedClaims));
  Require(!compact.Step(), "operation_journal_corrupt",
          "released claim compaction returned a row");
}

std::uint64_t SqliteOperationJournal::Impl::WriteInterruptRuleStateChanged(
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
             {"catalog_generation", CanonicalValue(std::to_string(cursor.catalog_generation))},
             {"source_id", CanonicalValue(cursor.source_id.value())},
             {"record_sequence", CanonicalValue(std::to_string(cursor.record_sequence))}})}});
  }
  CanonicalValue::Object fields{
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
  const auto sequence =
      AppendEvent("interrupt_rule_state_changed", change.occurred_at_ns, payload);
  // Reuse this journal's existing retained-history bound. Current truth comes
  // from the atomic Kernel baseline, never from assuming old rows still exist.
  Statement compact(
      database_,
      "DELETE FROM journal_records WHERE "
      "record_kind='interrupt_rule_state_changed' "
      "AND sequence NOT IN (SELECT sequence FROM journal_records WHERE "
      "record_kind='interrupt_rule_state_changed' ORDER BY sequence DESC LIMIT ?)");
  compact.Bind(1, static_cast<std::int64_t>(history_capacity_));
  Require(!compact.Step(), "operation_journal_corrupt",
          "global rule-state compaction returned a row");
  return sequence;
}

// Active TCP: one confirmed Active TCP change. Sparse like the rule-state
// rows; current truth is the robot owner's baseline (with this row's
// sequence), never old rows still kept.
std::uint64_t SqliteOperationJournal::Impl::WriteActiveTcpChanged(
    const ActiveTcpChangeDTO& change) {
  Require(!change.component_id.empty() &&
              (!change.entry || valid_tcp_entry_reference(*change.entry)) &&
              (!change.previous || valid_tcp_entry_reference(*change.previous)) &&
              change.registration_generation != 0U &&
              change.occurred_at_ns >= 0,
          "operation_journal_record_invalid", "Active TCP change is invalid");
  const auto entry_value = [](const std::optional<TcpEntryReferenceDTO>& entry) {
    return entry ? EncodeTcpEntryReference(*entry) : CanonicalValue(nullptr);
  };
  const auto payload = content_identity::canonical_json(CanonicalValue(CanonicalValue::Object{
      {"api_version", CanonicalValue("manyforge.active_tcp_change.v3")},
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
  const auto sequence = AppendEvent("active_tcp_changed", change.occurred_at_ns, payload);
  // The journal's existing retained-history bound, as for rule-state rows.
  Statement compact(
      database_,
      "DELETE FROM journal_records WHERE record_kind='active_tcp_changed' "
      "AND sequence NOT IN (SELECT sequence FROM journal_records WHERE "
      "record_kind='active_tcp_changed' ORDER BY sequence DESC LIMIT ?)");
  compact.Bind(1, static_cast<std::int64_t>(history_capacity_));
  Require(!compact.Step(), "operation_journal_corrupt",
          "Active TCP change compaction returned a row");
  return sequence;
}

SqliteOperationJournal::SqliteOperationJournal(
    std::filesystem::path database_path, CellId cell_id, std::size_t history_capacity,
    std::function<std::string(std::int64_t)> wall_time_text,
    std::function<void(std::string_view)> critical_section_observer)
    : impl_(std::make_unique<Impl>(std::move(database_path), std::move(cell_id), history_capacity,
                                   std::move(wall_time_text),
                                   std::move(critical_section_observer))) {}

SqliteOperationJournal::~SqliteOperationJournal() = default;

Result<void> SqliteOperationJournal::BeginStart(const ApplicationStartId application_start_id,
                                                std::int64_t started_at_ns,
                                                std::size_t request_capacity) {
  const auto begun = AsResult<bool>([&] {
    impl_->BeginStart(application_start_id, started_at_ns, request_capacity);
    return true;
  });
  if (!begun.ok()) return Result<void>::Err(begun.error());
  return Result<void>::Ok();
}

JournalBatchResult SqliteOperationJournal::WriteBatch(const std::span<const JournalRecord> records,
                                                     const std::uint64_t records_dropped) {
  return impl_->WriteBatch(records, records_dropped);
}

Result<OperationJournalEventPageDTO> SqliteOperationJournal::ReadOperationalEvents(
    const std::uint64_t after_sequence,
    const std::size_t limit) {
  return AsResult<OperationJournalEventPageDTO>(
      [&] { return impl_->ReadEvents(after_sequence, limit); });
}

void SqliteOperationJournal::Close() {
  impl_->Close();
}

}  // namespace manyforge::core
