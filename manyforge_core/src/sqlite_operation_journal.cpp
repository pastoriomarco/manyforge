#include <sqlite3.h>

#include <algorithm>
#include <array>
#include <chrono>
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

constexpr std::string_view kSchema = R"sql(
CREATE TABLE journal_metadata (
  singleton INTEGER PRIMARY KEY CHECK (singleton = 1),
  schema_id TEXT NOT NULL,
  schema_version INTEGER NOT NULL,
  cell_id TEXT NOT NULL,
  history_capacity INTEGER NOT NULL
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
  CHECK ((record_kind IN ('stop_acceptance_projected','stop_terminal_projected',
                          'stop_surveillance_checkpoint','stop_fault_cleared')) =
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
  acceptance_payload TEXT NOT NULL,
  acceptance_digest TEXT NOT NULL,
  terminal_sequence INTEGER UNIQUE,
  terminal_payload TEXT,
  terminal_digest TEXT,
  surveillance_sequence INTEGER UNIQUE,
  surveillance_payload TEXT,
  surveillance_digest TEXT,
  clear_sequence INTEGER UNIQUE,
  clear_request_id TEXT,
  clear_payload TEXT,
  clear_digest TEXT,
  CHECK ((terminal_sequence IS NULL) = (terminal_payload IS NULL)),
  CHECK ((terminal_sequence IS NULL) = (terminal_digest IS NULL)),
  CHECK ((surveillance_sequence IS NULL) = (surveillance_payload IS NULL)),
  CHECK ((surveillance_sequence IS NULL) = (surveillance_digest IS NULL)),
  CHECK ((clear_sequence IS NULL) = (clear_request_id IS NULL)),
  CHECK ((clear_sequence IS NULL) = (clear_payload IS NULL)),
  CHECK ((clear_sequence IS NULL) = (clear_digest IS NULL)),
  PRIMARY KEY(stop_id, stop_generation),
  FOREIGN KEY(acceptance_sequence) REFERENCES journal_records(sequence),
  FOREIGN KEY(terminal_sequence) REFERENCES journal_records(sequence),
  FOREIGN KEY(surveillance_sequence) REFERENCES journal_records(sequence),
  FOREIGN KEY(clear_sequence) REFERENCES journal_records(sequence)
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
  FOREIGN KEY(owner_epoch) REFERENCES owner_epochs(owner_epoch),
  FOREIGN KEY(snapshot_sequence) REFERENCES journal_records(sequence)
) STRICT;
CREATE INDEX operation_snapshot_epoch
  ON operation_snapshots(owner_epoch, snapshot_sequence);
CREATE TABLE operation_participants (
  operation_id TEXT NOT NULL,
  participant_index INTEGER NOT NULL,
  participant_id TEXT NOT NULL,
  controller_goal_id TEXT NOT NULL,
  controller_goal_generation INTEGER NOT NULL,
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
)sql";

class JournalFailure final : public std::runtime_error {
 public:
  JournalFailure(std::string code, std::string message)
      : std::runtime_error(std::move(message)), code_(std::move(code)) {}

  [[nodiscard]] const std::string& code() const noexcept { return code_; }

 private:
  std::string code_;
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

  void Bind(int index, double value) {
    Check(database_, sqlite3_bind_double(statement_, index, value), "bind SQLite real");
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
  Require(value >= 0 && static_cast<std::uint64_t>(value) <= kMaximumSafeInteger,
          "operation_journal_record_invalid",
          std::string(label) + " is outside the lossless I-JSON range");
  return CanonicalValue(static_cast<double>(value));
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
    case OperationStateKind::kPaused:
      return "paused";
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
      OperationStateKind::kPaused,
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
    case OperationControlKind::kPause:
      return "pause";
    case OperationControlKind::kResume:
      return "resume";
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
  }
  return CanonicalValue(CanonicalValue::Object{
      {"id", CanonicalValue(std::string(reference_identity_value(*reference)))},
      {"pose", reference->pose.has_value() ? PoseValue(*reference->pose) : CanonicalValue(nullptr)},
      {"type", CanonicalValue(std::string(type))},
  });
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
          return CanonicalValue(CanonicalValue::Object{
              {"fallback_policy",
               CanonicalValue(value.fallback_policy == CartesianFallbackPolicyDTO::kFailClosed
                                  ? "fail_closed"
                                  : "allow_joint_space_fallback")},
              {"kind", CanonicalValue("cartesian_path")},
              {"max_angular_acceleration_rad_s2",
               CanonicalValue(value.max_angular_acceleration_rad_s2)},
              {"max_angular_speed_rad_s", CanonicalValue(value.max_angular_speed_rad_s)},
              {"max_linear_acceleration_mps2", CanonicalValue(value.max_linear_acceleration_mps2)},
              {"max_linear_speed_mps", CanonicalValue(value.max_linear_speed_mps)},
              {"max_rotation_step_rad", CanonicalValue(value.max_rotation_step_rad)},
              {"max_translation_step_m", CanonicalValue(value.max_translation_step_m)},
              {"speed_mode", CanonicalValue("bounded")},
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
  std::string_view planning_profile;
  switch (motion.planning_profile) {
    case PlanningProfileDTO::kFirstFeasible:
      planning_profile = "first_feasible";
      break;
    case PlanningProfileDTO::kBalanced:
      planning_profile = "balanced";
      break;
    case PlanningProfileDTO::kQualityBounded:
      planning_profile = "quality_bounded";
      break;
  }
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
  return CanonicalValue(CanonicalValue::Object{
      {"base_frame_role_id", CanonicalValue(motion.base_frame_role_id.value())},
      {"cache_mode", CanonicalValue(cache_mode)},
      {"contact_policy_id", OptionalIdentity(motion.contact_policy_id)},
      {"kind", CanonicalValue("motion")},
      {"motion", MotionSpecValue(motion.motion)},
      {"motion_interrupt_scopes", CanonicalValue(CanonicalValue::Array{})},
      {"move_id", CanonicalValue(motion.move_id.value())},
      {"offset_reference", ReferenceValue(motion.offset_reference)},
      {"per_joint_dynamics_limits", CanonicalValue(std::move(dynamics))},
      {"planning_profile", CanonicalValue(std::string(planning_profile))},
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
      {"tcp_frame_role_id", CanonicalValue(motion.tcp_frame_role_id.value())},
      {"timeouts", CanonicalValue(CanonicalValue::Object{
                       {"execution_timeout_s", CanonicalValue(motion.timeouts.execution_timeout_s)},
                       {"planning_timeout_s", CanonicalValue(motion.timeouts.planning_timeout_s)},
                   })},
      {"tolerances",
       CanonicalValue(CanonicalValue::Object{
           {"joint_position_tol_rad", CanonicalValue(motion.tolerances.joint_position_tol_rad)},
           {"rotation_tol_rad", CanonicalValue(motion.tolerances.rotation_tol_rad)},
           {"translation_tol_m", CanonicalValue(motion.tolerances.translation_tol_m)},
       })},
  });
}

[[nodiscard]] CanonicalValue CapabilityPayloadValue(const CapabilityCommandIntentDTO& command) {
  CanonicalValue command_value = std::visit(
      [](const auto& value) {
        return CanonicalValue(CanonicalValue::Object{
            {"kind", CanonicalValue("gripper_position")},
            {"max_effort", CoordinateEffortValue(value.max_effort)},
            {"position", CoordinatePositionValue(value.position)},
        });
      },
      command.command_value);
  CanonicalValue::Array options;
  options.reserve(command.caller_options.size());
  for (const auto& option : command.caller_options) {
    options.emplace_back(CanonicalValue::Object{
        {"option_id", CanonicalValue(option.option_id.value())},
        {"value_s", CanonicalValue(option.value_s)},
    });
  }
  return CanonicalValue(CanonicalValue::Object{
      {"caller_options", CanonicalValue(std::move(options))},
      {"command_schema_id", CanonicalValue(command.command_schema_id.value())},
      {"command_schema_revision", CanonicalValue(command.command_schema_revision.value())},
      {"command_value", std::move(command_value)},
      {"completion_timeout_s", CanonicalValue(command.completion_timeout_s)},
      {"kind", CanonicalValue("capability_command")},
  });
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

[[nodiscard]] CanonicalValue OperationIntentValue(const OperationIntentDTO& intent) {
  CanonicalValue::Array participants;
  participants.reserve(intent.participants.size());
  for (const auto& participant : intent.participants) {
    participants.emplace_back(CanonicalValue::Object{
        {"action_span", ActionSpanValue(participant.action_span)},
        {"component_role_id", CanonicalValue(participant.component_role_id.value())},
        {"payload",
         std::holds_alternative<MotionParticipantIntentDTO>(participant.payload)
             ? MotionPayloadValue(std::get<MotionParticipantIntentDTO>(participant.payload))
             : CapabilityPayloadValue(std::get<CapabilityCommandIntentDTO>(participant.payload))},
        {"required_capability_id", CanonicalValue(participant.required_capability_id.value())},
        {"target_role_id", CanonicalValue(participant.target_role_id.value())},
    });
  }
  return CanonicalValue(CanonicalValue::Object{
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
  });
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
  CanonicalValue::Array participants;
  participants.reserve(state.participants.size());
  for (const auto& participant : state.participants) {
    participants.emplace_back(CanonicalValue::Object{
        {"capability_id", CanonicalValue(participant.capability_id.value())},
        {"capability_provider_binding_id",
         OptionalIdentity(participant.capability_provider_binding_id)},
        {"capability_provider_binding_revision",
         OptionalIdentity(participant.capability_provider_binding_revision)},
        {"command_target_id", CanonicalValue(participant.command_target_id.value())},
        {"control_binding_id", CanonicalValue(participant.control_binding_id.value())},
        {"control_binding_revision", CanonicalValue(participant.control_binding_revision.value())},
        {"control_domain_id", CanonicalValue(participant.control_domain_id.value())},
        {"control_domain_revision", CanonicalValue(participant.control_domain_revision.value())},
        {"controllable_component_id",
         CanonicalValue(participant.controllable_component_id.value())},
        {"controller_goal_generation",
         Number(participant.controller_goal_generation.value(), "controller goal generation")},
        {"controller_goal_id", CanonicalValue(participant.controller_goal_id.value())},
        {"participant_id", CanonicalValue(participant.participant_id.value())},
        {"payload_kind", CanonicalValue(std::string(PayloadKindName(participant.payload_kind)))},
        {"progress", CanonicalValue(participant.progress)},
        {"resources", StringArray(participant.resources)},
        {"state", CanonicalValue(std::string(OperationStateName(participant.state)))},
        {"terminal_error", ErrorValue(participant.terminal_error)},
    });
  }
  return CanonicalValue(CanonicalValue::Object{
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
  });
}

[[nodiscard]] CanonicalValue ControlRequestValue(const OperationControlRequestDTO& request) {
  return CanonicalValue(CanonicalValue::Object{
      {"expected_operation_revision",
       Number(request.expected_operation_revision.value(), "expected operation revision")},
      {"operation_id", CanonicalValue(request.operation_id.value())},
      {"reason", OptionalText(request.reason)},
      {"request_attribution", RequestAttributionValue(request.attribution)},
      {"request_context", RequestContextValue(request.request_context)},
  });
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
          } else {
            rows.emplace_back(CanonicalValue::Object{
                {"expected_graph_revision", CanonicalValue(target.expected_graph_revision.value())},
                {"expected_operation_resource_generation",
                 Number(target.expected_operation_resource_generation,
                        "Stop expected operation-resource generation")},
                {"expected_stop_generation",
                 Number(target.expected_stop_generation.value(), "Stop expected generation")},
                {"resource_id", CanonicalValue(target.resource_id.value())},
            });
          }
        }
        const char* kind = nullptr;
        if constexpr (std::is_same_v<Targets, std::vector<StopOperationTargetDTO>>) {
          kind = "operations";
        } else if constexpr (std::is_same_v<Targets, std::vector<StopComponentTargetDTO>>) {
          kind = "components";
        } else {
          kind = "resources";
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
        {"remaining_outer_duration_ns_at_join",
         Timestamp(predecessor.remaining_outer_duration_ns_at_join,
                   "predecessor remaining Stop duration")},
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

  CanonicalValue::Array participant_budgets;
  participant_budgets.reserve(accepted.participant_budget_allocations.size());
  for (const auto& participant : accepted.participant_budget_allocations) {
    CanonicalValue::Array phases;
    phases.reserve(participant.phases.size());
    for (const auto& phase : participant.phases) {
      phases.emplace_back(CanonicalValue::Object{
          {"fixed_deadline_offset_ns",
           Timestamp(phase.fixed_deadline_offset_ns, "fixed Stop phase offset")},
          {"phase_id", CanonicalValue(phase.phase_id.value())},
          {"remaining_deadline_offset_ns",
           Timestamp(phase.remaining_deadline_offset_ns, "remaining Stop phase offset")},
      });
    }
    participant_budgets.emplace_back(CanonicalValue::Object{
        {"operation_id", CanonicalValue(participant.operation_id.value())},
        {"origin_stop_generation",
         Number(participant.origin_stop_generation.value(), "participant origin Stop generation")},
        {"origin_stop_id", CanonicalValue(participant.origin_stop_id.value())},
        {"participant_id", CanonicalValue(participant.participant_id.value())},
        {"participant_stop_invocation_plan_digest",
         CanonicalValue(participant.participant_stop_invocation_plan_digest.value())},
        {"phases", CanonicalValue(std::move(phases))},
    });
  }

  return CanonicalValue(CanonicalValue::Object{
      {"accepted_at_ns", Timestamp(accepted.accepted_at_ns, "Stop accepted timestamp")},
      {"affected_operation_revisions", CanonicalValue(std::move(affected_operations))},
      {"captured_scope_digest", CanonicalValue(accepted.captured_scope_digest.value())},
      {"fixed_kernel_completion_reserve_ns", Timestamp(accepted.fixed_kernel_completion_reserve_ns,
                                                       "fixed Kernel Stop completion reserve")},
      {"fixed_outer_duration_ns",
       Timestamp(accepted.fixed_outer_duration_ns, "fixed Stop outer duration")},
      {"joined_predecessor_stops", CanonicalValue(std::move(predecessors))},
      {"participant_budget_allocations", CanonicalValue(std::move(participant_budgets))},
      {"remaining_outer_duration_ns",
       Timestamp(accepted.remaining_outer_duration_ns, "remaining Stop outer duration")},
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
      {"outer_deadline_ns", Timestamp(acceptance.outer_deadline_ns, "Stop outer deadline")},
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
  return CanonicalValue(CanonicalValue::Object{
      {"operations", CanonicalValue(std::move(operations))},
      {"resources", StringArray(affected_state.resources)},
  });
}

[[nodiscard]] CanonicalValue EmergencyStopAcceptanceProjectionValue(
    const EmergencyStopAcceptanceRecordDTO& acceptance,
    const EmergencyStopProjectedIntentDTO& intent, const StopAcceptedDTO& accepted,
    const EmergencyStopAffectedStateSnapshotDTO& affected_state) {
  return CanonicalValue(CanonicalValue::Object{
      {"acceptance", EmergencyStopAcceptanceRecordValue(acceptance)},
      {"accepted", StopAcceptedValue(accepted)},
      {"affected_state", EmergencyStopAffectedStateValue(affected_state)},
      {"api_version", CanonicalValue("manyforge.stop_acceptance_projection.v2")},
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
      {"api_version", CanonicalValue("manyforge.operation_journal_record.v1")},
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
      static_cast<std::uint64_t>(state.updated_at_ns) > kMaximumSafeInteger ||
      (state.active_stop_id.has_value() != state.active_stop_generation.has_value())) {
    return false;
  }
  for (const auto& participant : state.participants) {
    if (participant.participant_id.empty() || participant.controller_goal_id.empty() ||
        !participant.controller_goal_generation.nonzero() ||
        participant.controllable_component_id.empty() || participant.command_target_id.empty() ||
        participant.capability_id.empty() || participant.control_domain_id.empty() ||
        participant.control_domain_revision.empty() || participant.control_binding_id.empty() ||
        participant.control_binding_revision.empty() || !std::isfinite(participant.progress) ||
        participant.progress < 0.0 || participant.progress > 1.0 || participant.resources.empty() ||
        participant.resources.size() > kMaxKernelResourceSlots ||
        std::any_of(participant.resources.begin(), participant.resources.end(),
                    [](const auto& resource) { return resource.empty(); })) {
      return false;
    }
    const std::set<ResourceId> unique(participant.resources.begin(), participant.resources.end());
    if (unique.size() != participant.resources.size() ||
        participant.capability_provider_binding_id.has_value() !=
            participant.capability_provider_binding_revision.has_value()) {
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
              acceptance.outer_deadline_ns > acceptance.accepted_at_ns &&
              static_cast<std::uint64_t>(acceptance.outer_deadline_ns) <= kMaximumSafeInteger &&
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
              accepted.remaining_outer_duration_ns ==
                  acceptance.outer_deadline_ns - acceptance.accepted_at_ns &&
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

  for (std::size_t index = 0U; index < affected_state.operations.size(); ++index) {
    const auto& state = affected_state.operations[index];
    const auto& affected = accepted.affected_operation_revisions[index];
    RequireState(state);
    Require(state.operation_id == affected.operation_id &&
                state.operation_revision == affected.operation_revision_after_acceptance,
            "operation_journal_record_invalid",
            "affected operation state does not match its accepted revision");

    const auto first_budget =
        std::lower_bound(accepted.participant_budget_allocations.begin(),
                         accepted.participant_budget_allocations.end(), state.operation_id,
                         [](const auto& budget, const OperationId& operation_id) {
                           return budget.operation_id < operation_id;
                         });
    Require(first_budget != accepted.participant_budget_allocations.end() &&
                first_budget->operation_id == state.operation_id,
            "operation_journal_record_invalid",
            "affected operation has no origin-tagged participant budget");
    const auto origin_stop_id = first_budget->origin_stop_id;
    const auto origin_stop_generation = first_budget->origin_stop_generation;
    Require(
        std::all_of(first_budget,
                    std::upper_bound(first_budget, accepted.participant_budget_allocations.end(),
                                     state.operation_id,
                                     [](const OperationId& operation_id, const auto& budget) {
                                       return operation_id < budget.operation_id;
                                     }),
                    [&](const auto& budget) {
                      return budget.origin_stop_id == origin_stop_id &&
                             budget.origin_stop_generation == origin_stop_generation;
                    }) &&
            state.active_stop_id == origin_stop_id &&
            state.active_stop_generation == origin_stop_generation,
        "operation_journal_record_invalid",
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

[[nodiscard]] const std::set<std::string>& ExpectedSchemaObjects() {
  static const std::set<std::string> names{
      "index:journal_operation_sequence",
      "index:journal_request_identity",
      "index:journal_stop_acceptance_identity",
      "index:journal_stop_terminal_identity",
      "index:one_active_owner_epoch",
      "index:operation_snapshot_epoch",
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

[[nodiscard]] OperationStateDTO LoadOperationState(sqlite3* database,
                                                   const OperationId& operation_id) {
  Statement snapshot(database,
                     "SELECT operation_revision,program_binding_id,program_run_id,"
                     "coordination_policy,state,updated_at_ns,active_stop_id,"
                     "active_stop_generation,runtime_scale_generation "
                     "FROM operation_snapshots WHERE operation_id=?");
  snapshot.Bind(1, operation_id.value());
  Require(snapshot.Step(), "operation_journal_corrupt", "operation snapshot is missing");
  const auto revision = PositiveIdentifier(snapshot.Integer(0), "operation revision");
  const auto coordination = CoordinationFromName(snapshot.Text(3));
  const auto operation_state = OperationStateFromName(snapshot.Text(4));
  Require(coordination.has_value() && operation_state.has_value(), "operation_journal_corrupt",
          "operation snapshot contains an unknown closed-enum value");
  const auto updated_at_ns = snapshot.Integer(5);
  Require(updated_at_ns >= 0 && static_cast<std::uint64_t>(updated_at_ns) <= kMaximumSafeInteger,
          "operation_journal_corrupt", "operation snapshot timestamp is invalid");
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
  };
  Require(!snapshot.Step(), "operation_journal_corrupt",
          "operation snapshot identity is not unique");

  Statement participants(database,
                         "SELECT participant_index,participant_id,controller_goal_id,"
                         "controller_goal_generation,controllable_component_id,command_target_id,"
                         "capability_id,control_domain_id,control_domain_revision,"
                         "control_binding_id,control_binding_revision,"
                         "capability_provider_binding_id,capability_provider_binding_revision,"
                         "payload_kind,state,progress,terminal_error_code,terminal_error_message "
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
    const bool has_provider_id = !participants.IsNull(11);
    const bool has_provider_revision = !participants.IsNull(12);
    Require(has_provider_id == has_provider_revision, "operation_journal_corrupt",
            "operation participant has an incomplete provider binding");
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
            PositiveIdentifier(participants.Integer(3), "controller goal generation")),
        ControllableComponentId(participants.Text(4)),
        CommandTargetId(participants.Text(5)),
        CapabilityId(participants.Text(6)),
        ControlDomainId(participants.Text(7)),
        ControlDomainRevision(participants.Text(8)),
        ControlBindingId(participants.Text(9)),
        ControlBindingRevision(participants.Text(10)),
        has_provider_id ? std::optional<CapabilityProviderBindingId>(
                              CapabilityProviderBindingId(participants.Text(11)))
                        : std::nullopt,
        has_provider_revision ? std::optional<CapabilityProviderBindingRevision>(
                                    CapabilityProviderBindingRevision(participants.Text(12)))
                              : std::nullopt,
        *payload_kind,
        *participant_state,
        participants.Real(15),
        std::move(resources),
        std::move(terminal_error),
    });
    ++expected_index;
  }
  RequireState(state);
  return state;
}

void StoreOperationSnapshot(sqlite3* database, const RequestOwnerEpoch& owner_epoch,
                            const OperationStateDTO& state, std::uint64_t snapshot_sequence,
                            std::string_view disposition) {
  RequireState(state);
  Require(snapshot_sequence > 0U && snapshot_sequence <= kMaximumSafeInteger,
          "operation_journal_record_invalid", "operation journal sequence is invalid");
  Statement upsert(database,
                   "INSERT INTO operation_snapshots("
                   "operation_id,owner_epoch,operation_revision,program_binding_id,"
                   "program_run_id,coordination_policy,state,updated_at_ns,active_stop_id,"
                   "active_stop_generation,runtime_scale_generation,disposition,"
                   "snapshot_sequence) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?) "
                   "ON CONFLICT(operation_id) DO UPDATE SET owner_epoch=excluded.owner_epoch,"
                   "operation_revision=excluded.operation_revision,"
                   "program_binding_id=excluded.program_binding_id,"
                   "program_run_id=excluded.program_run_id,"
                   "coordination_policy=excluded.coordination_policy,state=excluded.state,"
                   "updated_at_ns=excluded.updated_at_ns,active_stop_id=excluded.active_stop_id,"
                   "active_stop_generation=excluded.active_stop_generation,"
                   "runtime_scale_generation=excluded.runtime_scale_generation,"
                   "disposition=excluded.disposition,"
                   "snapshot_sequence=excluded.snapshot_sequence");
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
  Require(!upsert.Step(), "operation_journal_corrupt",
          "operation snapshot upsert returned an unexpected row");

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
        "operation_id,participant_index,participant_id,controller_goal_id,"
        "controller_goal_generation,controllable_component_id,command_target_id,"
        "capability_id,control_domain_id,control_domain_revision,control_binding_id,"
        "control_binding_revision,capability_provider_binding_id,"
        "capability_provider_binding_revision,payload_kind,state,progress,"
        "terminal_error_code,terminal_error_message) "
        "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)");
    insert_participant.Bind(1, state.operation_id.value());
    insert_participant.Bind(2, static_cast<std::int64_t>(index));
    insert_participant.Bind(3, participant.participant_id.value());
    insert_participant.Bind(4, participant.controller_goal_id.value());
    insert_participant.Bind(
        5, static_cast<std::int64_t>(participant.controller_goal_generation.value()));
    insert_participant.Bind(6, participant.controllable_component_id.value());
    insert_participant.Bind(7, participant.command_target_id.value());
    insert_participant.Bind(8, participant.capability_id.value());
    insert_participant.Bind(9, participant.control_domain_id.value());
    insert_participant.Bind(10, participant.control_domain_revision.value());
    insert_participant.Bind(11, participant.control_binding_id.value());
    insert_participant.Bind(12, participant.control_binding_revision.value());
    if (participant.capability_provider_binding_id.has_value()) {
      insert_participant.Bind(13, participant.capability_provider_binding_id->value());
      insert_participant.Bind(14, participant.capability_provider_binding_revision->value());
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
                                     std::uint64_t snapshot_sequence) {
  Statement present(database, "SELECT COUNT(*) FROM operation_snapshots WHERE operation_id=?");
  present.Bind(1, state.operation_id.value());
  Require(present.Step(), "operation_journal_corrupt",
          "operation snapshot presence query returned no row");
  const auto count = present.Integer(0);
  Require(!present.Step() && (count == 0 || count == 1), "operation_journal_corrupt",
          "operation snapshot presence query is invalid");
  if (count == 0) {
    StoreOperationSnapshot(database, owner_epoch, state, snapshot_sequence, "current");
    return;
  }

  const auto current = LoadOperationState(database, state.operation_id);
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
  StoreOperationSnapshot(database, owner_epoch, state, snapshot_sequence, "current");
}

}  // namespace

class SqliteOperationJournal::Impl final {
 public:
  Impl(std::filesystem::path database_path, CellId cell_id, std::size_t history_capacity,
       std::function<std::string()> wall_time_text)
      : database_path_(ValidatePath(std::move(database_path))),
        cell_id_(std::move(cell_id)),
        history_capacity_(history_capacity),
        wall_time_text_(wall_time_text ? std::move(wall_time_text) : ProductionWallTimeText) {
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
                                        const OperationStateDTO& state);

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
                                   const StopCertificateDTO& certificate,
                                   const std::optional<StopFaultLatchDTO>& fault_latch);

  std::uint64_t RecordStopSurveillanceCheckpoint(const RequestOwnerEpoch& owner_epoch,
                                                 const StopPostTerminalSurveillanceDTO& checkpoint,
                                                 const StopFaultLatchDTO& fault_latch,
                                                 std::int64_t projected_at_ns);

  std::uint64_t RecordStopFaultCleared(const RequestOwnerEpoch& owner_epoch,
                                       const StopFaultRecoveryIntentDTO& recovery_intent,
                                       const StopFaultRecoveryResultDTO& recovery_result,
                                       const StopFaultLatchDTO& cleared_latch,
                                       const StopStatusDTO& resulting_stop_status);

  std::uint64_t CloseOwnerEpoch(const RequestOwnerEpoch& owner_epoch, std::int64_t closed_at_ns);

  std::uint64_t RecordRuntimeScaleAccepted(const RequestOwnerEpoch& owner_epoch,
                                           const RuntimeScaleRequestDTO& request,
                                           const RuntimeScaleAcceptedDTO& accepted,
                                           const OperationStateDTO& state,
                                           const RuntimeScaleStatusDTO& scale_status);

  std::uint64_t RecordOperationState(const RequestOwnerEpoch& owner_epoch,
                                     const OperationStateDTO& state,
                                     const RuntimeScaleStatusDTO* scale_status,
                                     const char* transition_reason);

  OperationJournalEventPageDTO ReadEvents(const RequestOwnerEpoch& owner_epoch,
                                          std::uint64_t after_sequence, std::size_t limit,
                                          bool operational_only = false);

 private:
  void RequireOpen() const {
    Require(!closed_ && database_ != nullptr, "operation_journal_closed",
            "operation journal database is closed");
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
    std::lock_guard lock(mutex_);
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

  std::uint64_t Append(const RequestOwnerEpoch& owner_epoch, std::string_view record_kind,
                       const std::optional<RequestId>& request_id,
                       const CanonicalValue& record_value, const OperationStateDTO& state,
                       std::int64_t occurred_at_ns, bool effect_pending,
                       std::string_view disposition);

  std::uint64_t AppendStopAcceptance(const RequestOwnerEpoch& owner_epoch,
                                     const EmergencyStopAcceptanceRecordDTO& acceptance,
                                     const CanonicalValue& record_value,
                                     const StopAcceptedDTO& accepted,
                                     const EmergencyStopAffectedStateSnapshotDTO& affected_state);

  std::uint64_t AppendStopTerminal(const RequestOwnerEpoch& owner_epoch,
                                   const StopJournalTerminalProjectionDTO& projection,
                                   const std::string& payload_json);

  std::uint64_t AppendStopSurveillanceCheckpoint(
      const RequestOwnerEpoch& owner_epoch, const StopJournalSurveillanceProjectionDTO& projection,
      const std::string& payload_json, std::int64_t projected_at_ns);

  std::filesystem::path database_path_;
  CellId cell_id_;
  std::size_t history_capacity_;
  std::function<std::string()> wall_time_text_;
  std::mutex mutex_;
  sqlite3* database_{};
  bool closed_{};
};

std::uint64_t SqliteOperationJournal::Impl::Append(const RequestOwnerEpoch& owner_epoch,
                                                   std::string_view record_kind,
                                                   const std::optional<RequestId>& request_id,
                                                   const CanonicalValue& record_value,
                                                   const OperationStateDTO& state,
                                                   std::int64_t occurred_at_ns, bool effect_pending,
                                                   std::string_view disposition) {
  Require(!owner_epoch.empty() && !record_kind.empty() &&
              (!request_id.has_value() || !request_id->empty()) && occurred_at_ns >= 0 &&
              static_cast<std::uint64_t>(occurred_at_ns) <= kMaximumSafeInteger,
          "operation_journal_record_invalid", "operation journal record identity is invalid");
  RequireState(state);
  const auto payload_json = content_identity::canonical_json(record_value);
  Require(payload_json.size() <= kMaximumRecordJsonBytes, "operation_journal_record_too_large",
          "operation journal record exceeds the fixed payload bound");
  const auto payload_digest = content_identity::sha256_digest(payload_json);

  std::lock_guard lock(mutex_);
  RequireOpen();
  const auto occurred_at_wall = wall_time_text_();
  ValidateWallTimeText(occurred_at_wall);
  Transaction transaction(database_);
  Statement active(database_,
                   "SELECT request_capacity FROM owner_epochs "
                   "WHERE owner_epoch=? AND active=1");
  active.Bind(1, owner_epoch.value());
  Require(active.Step(), "operation_journal_owner_epoch_stale",
          "operation journal write names a non-active owner epoch");
  const auto request_capacity = active.Integer(0);
  Require(request_capacity > 0 && !active.Step(), "operation_journal_corrupt",
          "active operation journal owner epoch is invalid");
  if (request_id.has_value()) {
    Statement request_count(database_,
                            "SELECT COUNT(*) FROM journal_records "
                            "WHERE owner_epoch=? AND request_id IS NOT NULL "
                            "AND record_kind<>'stop_acceptance_projected'");
    request_count.Bind(1, owner_epoch.value());
    Require(request_count.Step(), "operation_journal_corrupt",
            "operation journal request count returned no row");
    const auto count = request_count.Integer(0);
    Require(!request_count.Step() && count >= 0, "operation_journal_corrupt",
            "operation journal request count is invalid");
    Require(count < request_capacity, "request_ledger_full",
            "operation journal request ledger reached its fixed bound");
    Statement existing(database_,
                       "SELECT record_kind,payload_digest FROM journal_records "
                       "WHERE owner_epoch=? AND request_id=? "
                       "AND record_kind<>'stop_acceptance_projected'");
    existing.Bind(1, owner_epoch.value());
    existing.Bind(2, request_id->value());
    Require(!existing.Step(), "request_id_conflict",
            "operation journal request identity is already recorded");
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
  StoreOperationSnapshot(database_, owner_epoch, state, bounded_sequence, disposition);

  if (!request_id.has_value() && record_kind == "state_observed") {
    Statement coalesce(database_,
                       "DELETE FROM journal_records WHERE operation_id=? AND request_id IS NULL "
                       "AND record_kind='state_observed' AND sequence<>? AND sequence NOT IN ("
                       "SELECT snapshot_sequence FROM operation_snapshots)");
    coalesce.Bind(1, state.operation_id.value());
    coalesce.Bind(2, static_cast<std::int64_t>(sequence));
    Require(!coalesce.Step(), "operation_journal_corrupt",
            "operation journal state coalescing returned an unexpected row");
  }
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

  std::lock_guard lock(mutex_);
  RequireOpen();
  const auto occurred_at_wall = wall_time_text_();
  ValidateWallTimeText(occurred_at_wall);
  Transaction transaction(database_);
  Statement active(database_, "SELECT COUNT(*) FROM owner_epochs WHERE owner_epoch=? AND active=1");
  active.Bind(1, owner_epoch.value());
  Require(active.Step() && active.Integer(0) == 1 && !active.Step(),
          "operation_journal_owner_epoch_stale",
          "Stop acceptance projection names a non-active owner epoch");

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
    const auto acceptance_projection = EncodeStopJournalAcceptanceProjection(
        StopJournalAcceptanceProjectionDTO{accepted, affected_state});
    Require(acceptance_projection.ok(), "operation_journal_record_invalid",
            "Stop acceptance recovery projection is invalid");
    Statement existing_projection(
        database_,
        "SELECT stop_generation,acceptance_sequence,acceptance_payload FROM stop_projections "
        "WHERE stop_id=? AND stop_generation=?");
    existing_projection.Bind(1, accepted.stop_id.value());
    existing_projection.Bind(2, static_cast<std::int64_t>(accepted.stop_generation.value()));
    Require(existing_projection.Step() &&
                PositiveIdentifier(existing_projection.Integer(0), "Stop generation") ==
                    accepted.stop_generation.value() &&
                PositiveIdentifier(existing_projection.Integer(1), "operation journal sequence") ==
                    sequence &&
                existing_projection.Text(2) == acceptance_projection.value() &&
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
  for (const auto& state : affected_state.operations) {
    StoreOperationSnapshotMonotonic(database_, owner_epoch, state, sequence);
  }
  const auto acceptance_projection = EncodeStopJournalAcceptanceProjection(
      StopJournalAcceptanceProjectionDTO{accepted, affected_state});
  Require(acceptance_projection.ok(), "operation_journal_record_invalid",
          "Stop acceptance recovery projection is invalid");
  Statement insert_projection(
      database_,
      "INSERT INTO stop_projections(stop_id,stop_generation,acceptance_sequence,"
      "acceptance_payload,acceptance_digest,terminal_sequence,terminal_payload,terminal_digest,"
      "surveillance_sequence,surveillance_payload,surveillance_digest,clear_sequence,"
      "clear_request_id,clear_payload,clear_digest) "
      "VALUES(?,?,?,?,?,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL)");
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

std::uint64_t SqliteOperationJournal::Impl::AppendStopSurveillanceCheckpoint(
    const RequestOwnerEpoch& owner_epoch, const StopJournalSurveillanceProjectionDTO& projection,
    const std::string& payload_json, const std::int64_t projected_at_ns) {
  Require(
      projected_at_ns >= 0 && static_cast<std::uint64_t>(projected_at_ns) <= kMaximumSafeInteger,
      "operation_journal_record_invalid", "Stop surveillance projection time is invalid");
  Require(payload_json.size() <= kMaximumRecordJsonBytes, "operation_journal_record_too_large",
          "Stop surveillance projection exceeds the fixed payload bound");
  const auto payload_digest = content_identity::sha256_digest(payload_json);
  std::lock_guard lock(mutex_);
  RequireOpen();
  const auto occurred_at_wall = wall_time_text_();
  ValidateWallTimeText(occurred_at_wall);
  Transaction transaction(database_);
  Statement active(database_, "SELECT COUNT(*) FROM owner_epochs WHERE owner_epoch=? AND active=1");
  active.Bind(1, owner_epoch.value());
  Require(active.Step() && active.Integer(0) == 1 && !active.Step(),
          "operation_journal_owner_epoch_stale",
          "Stop surveillance projection names a non-active owner epoch");

  Statement current(database_,
                    "SELECT terminal_payload,surveillance_sequence,surveillance_payload "
                    "FROM stop_projections WHERE stop_id=? AND stop_generation=?");
  current.Bind(1, projection.checkpoint.stop_id.value());
  current.Bind(2, static_cast<std::int64_t>(projection.checkpoint.stop_generation.value()));
  Require(current.Step() && !current.IsNull(0), "operation_journal_stop_terminal_missing",
          "Stop surveillance checkpoint has no durable terminal predecessor");
  auto terminal = DecodeStopJournalTerminalProjection(current.Text(0));
  Require(terminal.ok() &&
              terminal.value().certificate.stop_certificate_id ==
                  projection.checkpoint.stop_certificate.stop_certificate_id &&
              terminal.value().certificate.stop_certificate_revision ==
                  projection.checkpoint.stop_certificate.stop_certificate_revision,
          "operation_journal_stop_projection_conflict",
          "Stop surveillance checkpoint does not join durable terminal truth");
  if (!current.IsNull(1)) {
    const auto prior_sequence =
        PositiveIdentifier(current.Integer(1), "operation journal sequence");
    auto prior = DecodeStopJournalSurveillanceProjection(current.Text(2));
    Require(prior.ok(), "operation_journal_corrupt",
            "Stop surveillance recovery projection cannot be decoded");
    if (prior.value() == projection) {
      Require(!current.Step(), "operation_journal_corrupt",
              "Stop surveillance lookup returned duplicate rows");
      transaction.Commit();
      return prior_sequence;
    }
    Require(projection.checkpoint.checkpoint_generation >
                    prior.value().checkpoint.checkpoint_generation &&
                projection.checkpoint.surveillance_generation >
                    prior.value().checkpoint.surveillance_generation &&
                projection.checkpoint.continued_motion_count >=
                    prior.value().checkpoint.continued_motion_count &&
                projection.fault_latch.recovery_assessment_revision >=
                    prior.value().fault_latch.recovery_assessment_revision,
            "operation_journal_stop_projection_conflict",
            "Stop surveillance checkpoint regresses durable monotonic truth");
  }
  Require(!current.Step(), "operation_journal_corrupt",
          "Stop surveillance lookup returned duplicate rows");

  Statement insert(database_,
                   "INSERT INTO journal_records(owner_epoch,record_kind,request_id,stop_id,"
                   "stop_generation,operation_id,operation_revision,occurred_at_ns,"
                   "occurred_at_wall,effect_pending,payload_json,payload_digest) "
                   "VALUES(?,'stop_surveillance_checkpoint',NULL,?,?,NULL,NULL,?,?,?,?,?)");
  insert.Bind(1, owner_epoch.value());
  insert.Bind(2, projection.checkpoint.stop_id.value());
  insert.Bind(3, static_cast<std::int64_t>(projection.checkpoint.stop_generation.value()));
  insert.Bind(4, projected_at_ns);
  insert.Bind(5, occurred_at_wall);
  insert.Bind(6, static_cast<std::int64_t>(0));
  insert.Bind(7, payload_json);
  insert.Bind(8, payload_digest);
  Require(!insert.Step(), "operation_journal_corrupt",
          "Stop surveillance journal insert returned an unexpected row");
  const auto sequence =
      PositiveIdentifier(sqlite3_last_insert_rowid(database_), "operation journal sequence");
  Statement update(database_,
                   "UPDATE stop_projections SET surveillance_sequence=?,surveillance_payload=?,"
                   "surveillance_digest=? WHERE stop_id=? AND stop_generation=?");
  update.Bind(1, static_cast<std::int64_t>(sequence));
  update.Bind(2, payload_json);
  update.Bind(3, payload_digest);
  update.Bind(4, projection.checkpoint.stop_id.value());
  update.Bind(5, static_cast<std::int64_t>(projection.checkpoint.stop_generation.value()));
  Require(!update.Step() && sqlite3_changes(database_) == 1, "operation_journal_corrupt",
          "Stop surveillance durable projection update did not affect exactly one row");
  Statement prune_checkpoints(
      database_,
      "DELETE FROM journal_records WHERE sequence IN ("
      "SELECT sequence FROM journal_records WHERE record_kind='stop_surveillance_checkpoint' "
      "AND stop_id=? AND stop_generation=? ORDER BY sequence DESC LIMIT -1 OFFSET ?)");
  prune_checkpoints.Bind(1, projection.checkpoint.stop_id.value());
  prune_checkpoints.Bind(2,
                         static_cast<std::int64_t>(projection.checkpoint.stop_generation.value()));
  prune_checkpoints.Bind(
      3, static_cast<std::int64_t>(projection.checkpoint.policy.retained_checkpoint_limit));
  Require(!prune_checkpoints.Step(), "operation_journal_corrupt",
          "Stop surveillance checkpoint retention returned an unexpected row");
  transaction.Commit();
  return sequence;
}

std::uint64_t SqliteOperationJournal::Impl::AppendStopTerminal(
    const RequestOwnerEpoch& owner_epoch, const StopJournalTerminalProjectionDTO& projection,
    const std::string& payload_json) {
  Require(payload_json.size() <= kMaximumRecordJsonBytes, "operation_journal_record_too_large",
          "Stop terminal projection exceeds the fixed payload bound");
  const auto payload_digest = content_identity::sha256_digest(payload_json);

  std::lock_guard lock(mutex_);
  RequireOpen();
  const auto occurred_at_wall = wall_time_text_();
  ValidateWallTimeText(occurred_at_wall);
  Transaction transaction(database_);
  Statement active(database_, "SELECT COUNT(*) FROM owner_epochs WHERE owner_epoch=? AND active=1");
  active.Bind(1, owner_epoch.value());
  Require(active.Step() && active.Integer(0) == 1 && !active.Step(),
          "operation_journal_owner_epoch_stale",
          "Stop terminal projection names a non-active owner epoch");

  Statement acceptance(database_,
                       "SELECT terminal_sequence,terminal_payload FROM stop_projections "
                       "WHERE stop_id=? AND stop_generation=?");
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
    Statement predecessor_record(database_,
                                 "SELECT terminal_payload FROM stop_projections "
                                 "WHERE stop_id=? AND stop_generation=?");
    predecessor_record.Bind(1, predecessor.stop_id.value());
    predecessor_record.Bind(2, static_cast<std::int64_t>(predecessor.stop_generation.value()));
    Require(predecessor_record.Step() && !predecessor_record.IsNull(0),
            "operation_journal_stop_predecessor_unresolved",
            "Stop terminal predecessor has no durable terminal certificate truth");
    auto decoded_predecessor = DecodeStopJournalTerminalProjection(predecessor_record.Text(0));
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
  Statement update(database_,
                   "UPDATE stop_projections SET terminal_sequence=?,terminal_payload=?,"
                   "terminal_digest=? WHERE stop_id=? AND stop_generation=? "
                   "AND terminal_sequence IS NULL");
  update.Bind(1, static_cast<std::int64_t>(sequence));
  update.Bind(2, payload_json);
  update.Bind(3, payload_digest);
  update.Bind(4, projection.certificate.stop_id.value());
  update.Bind(5, static_cast<std::int64_t>(projection.certificate.stop_generation.value()));
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

  std::lock_guard lock(mutex_);
  RequireOpen();
  Statement active(database_, "SELECT COUNT(*) FROM owner_epochs WHERE owner_epoch=? AND active=1");
  active.Bind(1, owner_epoch.value());
  Require(active.Step() && active.Integer(0) == 1 && !active.Step(),
          "operation_journal_owner_epoch_stale",
          "operation event query names a non-active owner epoch");

  Statement query(
      database_,
      operational_only
          ? "SELECT sequence,record_kind,request_id,operation_id,"
            "operation_revision,occurred_at_ns,occurred_at_wall,effect_pending,"
            "payload_json,payload_digest FROM journal_records "
            "WHERE owner_epoch=? AND sequence>? AND record_kind IN "
            "('operation_accepted','control_accepted','stop_acceptance_projected',"
            "'runtime_scale_accepted','state_observed','host_replaced') "
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
                static_cast<std::uint64_t>(occurred_at_ns) <= kMaximumSafeInteger &&
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
  Require(!owner_epoch.empty() && started_at_ns >= 0 &&
              static_cast<std::uint64_t>(started_at_ns) <= kMaximumSafeInteger &&
              request_capacity > 0U && request_capacity <= kMaximumRequestCapacity &&
              !resource_inventory.empty() && resource_inventory.size() <= kMaxKernelResourceSlots,
          "operation_journal_owner_epoch_invalid",
          "operation journal owner epoch admission is invalid");
  const std::set<ResourceId> inventory(resource_inventory.begin(), resource_inventory.end());
  Require(inventory.size() == resource_inventory.size() &&
              std::none_of(resource_inventory.begin(), resource_inventory.end(),
                           [](const auto& resource) { return resource.empty(); }),
          "operation_journal_owner_epoch_invalid",
          "operation journal resource inventory is invalid");

  std::lock_guard lock(mutex_);
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
  Exec(database_, "UPDATE owner_epochs SET active=0 WHERE active=1");
  Statement insert_epoch(database_,
                         "INSERT INTO owner_epochs(owner_epoch,started_at_ns,request_capacity,"
                         "active) VALUES(?,?,?,1)");
  insert_epoch.Bind(1, owner_epoch.value());
  insert_epoch.Bind(2, started_at_ns);
  insert_epoch.Bind(3, static_cast<std::int64_t>(request_capacity));
  Require(!insert_epoch.Step(), "operation_journal_corrupt",
          "operation owner epoch insert returned an unexpected row");

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
    StoreOperationSnapshot(database_, owner_epoch, state, sequence, "orphaned_by_host_replacement");
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
                          "AND sequence NOT IN (SELECT surveillance_sequence FROM stop_projections "
                          "WHERE surveillance_sequence IS NOT NULL) "
                          "AND sequence NOT IN (SELECT clear_sequence FROM stop_projections "
                          "WHERE clear_sequence IS NOT NULL) "
                          "AND record_kind NOT IN "
                          "('stop_acceptance_projected','stop_terminal_projected')");
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
  recovery.replaced_unclosed_owner_epoch = std::move(replaced_unclosed_owner_epoch);
  Statement retained(database_,
                     "SELECT operation_id FROM operation_snapshots ORDER BY snapshot_sequence");
  while (retained.Step()) {
    recovery.historical_operations.push_back(
        LoadOperationState(database_, OperationId(retained.Text(0))));
  }
  Statement retained_stops(
      database_,
      "SELECT acceptance_payload,acceptance_digest,terminal_payload,terminal_digest,"
      "surveillance_payload,surveillance_digest,clear_payload,clear_digest "
      "FROM stop_projections ORDER BY stop_id,stop_generation");
  while (retained_stops.Step()) {
    const auto acceptance_payload = retained_stops.Text(0);
    Require(content_identity::sha256_digest(acceptance_payload) == retained_stops.Text(1),
            "operation_journal_corrupt",
            "Stop acceptance recovery projection digest does not match its bytes");
    auto acceptance = DecodeStopJournalAcceptanceProjection(acceptance_payload);
    Require(acceptance.ok(), "operation_journal_corrupt",
            "Stop acceptance recovery projection cannot be decoded");
    RecoveredStopJournalStateDTO recovered{std::move(acceptance.value().accepted),
                                           std::move(acceptance.value().affected_state),
                                           std::nullopt, std::nullopt, std::nullopt};
    const bool has_terminal_payload = !retained_stops.IsNull(2);
    Require(has_terminal_payload == !retained_stops.IsNull(3), "operation_journal_corrupt",
            "Stop terminal recovery projection presence is inconsistent");
    if (has_terminal_payload) {
      const auto terminal_payload = retained_stops.Text(2);
      Require(content_identity::sha256_digest(terminal_payload) == retained_stops.Text(3),
              "operation_journal_corrupt",
              "Stop terminal recovery projection digest does not match its bytes");
      auto terminal = DecodeStopJournalTerminalProjection(terminal_payload);
      Require(terminal.ok() && terminal.value().stop_status.accepted == recovered.accepted,
              "operation_journal_corrupt",
              "Stop terminal recovery projection cannot be decoded or joined to acceptance");
      recovered.terminal_certificate = std::move(terminal.value().certificate);
      recovered.fault_latch = std::move(terminal.value().fault_latch);
      recovered.surveillance = std::move(terminal.value().stop_status.surveillance);
    }
    const bool has_surveillance_payload = !retained_stops.IsNull(4);
    Require(has_surveillance_payload == !retained_stops.IsNull(5), "operation_journal_corrupt",
            "Stop surveillance recovery projection presence is inconsistent");
    if (has_surveillance_payload) {
      const auto payload = retained_stops.Text(4);
      Require(content_identity::sha256_digest(payload) == retained_stops.Text(5),
              "operation_journal_corrupt",
              "Stop surveillance recovery projection digest does not match its bytes");
      auto surveillance = DecodeStopJournalSurveillanceProjection(payload);
      Require(surveillance.ok() && recovered.terminal_certificate.has_value() &&
                  surveillance.value().checkpoint.stop_certificate.stop_certificate_id ==
                      recovered.terminal_certificate->stop_certificate_id &&
                  surveillance.value().checkpoint.stop_certificate.stop_certificate_revision ==
                      recovered.terminal_certificate->stop_certificate_revision,
              "operation_journal_corrupt",
              "Stop surveillance recovery projection cannot be decoded or joined");
      recovered.surveillance = std::move(surveillance.value().checkpoint);
      recovered.fault_latch = std::move(surveillance.value().fault_latch);
    }
    const bool has_clear_payload = !retained_stops.IsNull(6);
    Require(has_clear_payload == !retained_stops.IsNull(7), "operation_journal_corrupt",
            "Stop fault-clear recovery projection presence is inconsistent");
    if (has_clear_payload) {
      const auto payload = retained_stops.Text(6);
      Require(content_identity::sha256_digest(payload) == retained_stops.Text(7),
              "operation_journal_corrupt",
              "Stop fault-clear recovery projection digest does not match its bytes");
      auto cleared = DecodeStopJournalFaultClearProjection(payload);
      Require(cleared.ok() &&
                  cleared.value().resulting_stop_status.accepted == recovered.accepted &&
                  cleared.value().recovery_result.latch.state == StopFaultLatchStateDTO::kCleared,
              "operation_journal_corrupt",
              "Stop fault-clear recovery projection cannot be decoded or joined");
      recovered.terminal_certificate = std::move(cleared.value().certificate);
      recovered.fault_latch = std::move(cleared.value().recovery_result.latch);
      recovered.surveillance = std::move(cleared.value().resulting_stop_status.surveillance);
    }
    recovery.recovered_stops.push_back(std::move(recovered));
  }
  Require(recovery.recovered_stops.size() <= 64U, "operation_journal_history_full",
          "recovered Stop truth exceeds the fixed restart bound");
  recovery.unreconciled_resources.assign(unreconciled.begin(), unreconciled.end());
  transaction.Commit();
  return recovery;
}

std::uint64_t SqliteOperationJournal::Impl::RecordOperationAccepted(
    const RequestOwnerEpoch& owner_epoch, const OperationIntentDTO& intent,
    const OperationAcceptedDTO& accepted, const OperationStateDTO& state) {
  Require(intent.request_context.request_owner_epoch == owner_epoch &&
              intent.request_context.request_id == accepted.request_id &&
              accepted.replay_status == RequestReplayStatusDTO::kAcceptedNew &&
              accepted.operation_id == state.operation_id &&
              accepted.operation_revision == state.operation_revision &&
              accepted.accepted_at_ns == state.updated_at_ns &&
              accepted.participant_assignments.size() == state.participants.size(),
          "operation_journal_record_invalid",
          "operation acceptance does not match its durable intent or state");
  for (std::size_t index = 0; index < accepted.participant_assignments.size(); ++index) {
    Require(accepted.participant_assignments[index].input_index == index &&
                accepted.participant_assignments[index].participant_id ==
                    state.participants[index].participant_id,
            "operation_journal_record_invalid",
            "operation acceptance participant assignment is inconsistent");
  }
  return Append(owner_epoch, "operation_accepted", intent.request_context.request_id,
                RecordValue("operation_accepted", owner_epoch, OperationIntentValue(intent),
                            OperationAcceptedValue(accepted), state, nullptr, true),
                state, accepted.accepted_at_ns, true, "current");
}

std::uint64_t SqliteOperationJournal::Impl::RecordControlAccepted(
    const RequestOwnerEpoch& owner_epoch, const OperationControlRequestDTO& request,
    const OperationControlAcceptedDTO& accepted, const OperationStateDTO& state) {
  Require(request.request_context.request_owner_epoch == owner_epoch &&
              request.request_context.request_id == accepted.request_id &&
              request.operation_id == accepted.operation_id &&
              request.operation_id == state.operation_id &&
              request.expected_operation_revision == accepted.accepted_against_revision &&
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
    const StopCertificateDTO& certificate, const std::optional<StopFaultLatchDTO>& fault_latch) {
  Require(!owner_epoch.empty(), "operation_journal_record_invalid",
          "Stop terminal projection owner epoch is empty");
  Require(!stop_terminal_projection_field_error(stop_status, certificate, fault_latch).has_value(),
          "operation_journal_record_invalid", "Stop terminal projection is invalid");
  auto recorded_status = stop_status;
  auto recorded_latch = fault_latch;
  recorded_status.terminal_projection = StopDurableProjectionStatusDTO{
      StopDurableProjectionStateDTO::kRecorded, std::nullopt, std::nullopt};
  if (recorded_latch.has_value()) {
    recorded_latch->terminal_projection = StopDurableProjectionStatusDTO{
        StopDurableProjectionStateDTO::kRecorded, std::nullopt, std::nullopt};
  }
  recorded_status.fault_latch = recorded_latch;
  StopJournalTerminalProjectionDTO projection{std::move(recorded_status), certificate,
                                              std::move(recorded_latch)};
  auto encoded = EncodeStopJournalTerminalProjection(projection);
  Require(encoded.ok(), "operation_journal_record_invalid", "Stop terminal projection is invalid");
  return AppendStopTerminal(owner_epoch, projection, encoded.value());
}

std::uint64_t SqliteOperationJournal::Impl::RecordStopSurveillanceCheckpoint(
    const RequestOwnerEpoch& owner_epoch, const StopPostTerminalSurveillanceDTO& checkpoint,
    const StopFaultLatchDTO& fault_latch, const std::int64_t projected_at_ns) {
  Require(!owner_epoch.empty() && checkpoint.owner_epoch == owner_epoch &&
              checkpoint.checkpoint_projection.state == StopDurableProjectionStateDTO::kPending &&
              fault_latch.state != StopFaultLatchStateDTO::kCleared,
          "operation_journal_record_invalid", "Stop surveillance checkpoint is invalid");
  auto recorded = checkpoint;
  recorded.checkpoint_projection = StopDurableProjectionStatusDTO{
      StopDurableProjectionStateDTO::kRecorded, std::nullopt, std::nullopt};
  auto encoded = EncodeStopJournalSurveillanceProjection(
      StopJournalSurveillanceProjectionDTO{recorded, fault_latch});
  Require(encoded.ok(), "operation_journal_record_invalid",
          "Stop surveillance checkpoint projection is invalid");
  return AppendStopSurveillanceCheckpoint(
      owner_epoch, StopJournalSurveillanceProjectionDTO{std::move(recorded), fault_latch},
      encoded.value(), projected_at_ns);
}

std::uint64_t SqliteOperationJournal::Impl::RecordStopFaultCleared(
    const RequestOwnerEpoch& owner_epoch, const StopFaultRecoveryIntentDTO& recovery_intent,
    const StopFaultRecoveryResultDTO& recovery_result, const StopFaultLatchDTO& cleared_latch,
    const StopStatusDTO& resulting_stop_status) {
  Require(
      !owner_epoch.empty() && recovery_intent.request_context.request_owner_epoch == owner_epoch &&
          recovery_intent.request_context.request_id == recovery_result.request_id &&
          recovery_result.accepted && !recovery_result.error.has_value() &&
          recovery_result.latch == cleared_latch && resulting_stop_status.fault_latch.has_value() &&
          *resulting_stop_status.fault_latch == cleared_latch &&
          cleared_latch.state == StopFaultLatchStateDTO::kCleared &&
          cleared_latch.clear_projection.state == StopDurableProjectionStateDTO::kRecorded &&
          stop_status_field_error(resulting_stop_status) == std::nullopt,
      "operation_journal_record_invalid", "Stop fault-clear projection is invalid");
  std::lock_guard lock(mutex_);
  RequireOpen();
  const auto occurred_at_wall = wall_time_text_();
  ValidateWallTimeText(occurred_at_wall);
  Transaction transaction(database_);
  Statement current(database_,
                    "SELECT terminal_payload,surveillance_payload,clear_sequence,"
                    "clear_request_id,clear_payload FROM stop_projections "
                    "WHERE stop_id=? AND stop_generation=?");
  current.Bind(1, cleared_latch.stop_id.value());
  current.Bind(2, static_cast<std::int64_t>(cleared_latch.stop_generation.value()));
  Require(current.Step() && !current.IsNull(0), "operation_journal_stop_terminal_missing",
          "Stop fault clear has no durable terminal predecessor");
  auto terminal = DecodeStopJournalTerminalProjection(current.Text(0));
  Require(terminal.ok() && terminal.value().fault_latch.has_value() &&
              terminal.value().fault_latch->state != StopFaultLatchStateDTO::kCleared &&
              terminal.value().certificate.stop_certificate_id ==
                  recovery_intent.expected_stop_certificate_id &&
              terminal.value().certificate.stop_certificate_revision ==
                  recovery_intent.expected_stop_certificate_revision,
          "operation_journal_stop_projection_conflict",
          "Stop fault clear does not join durable terminal truth");
  auto prior_latch = *terminal.value().fault_latch;
  auto expected_status = terminal.value().stop_status;
  if (current.IsNull(1)) {
    Require(!resulting_stop_status.surveillance.has_value(),
            "operation_journal_stop_projection_conflict",
            "Stop fault clear invents surveillance truth");
  } else {
    auto surveillance = DecodeStopJournalSurveillanceProjection(current.Text(1));
    Require(surveillance.ok() && resulting_stop_status.surveillance.has_value() &&
                *resulting_stop_status.surveillance == surveillance.value().checkpoint &&
                terminal.value().fault_latch->latch_id == surveillance.value().fault_latch.latch_id,
            "operation_journal_stop_projection_conflict",
            "Stop fault clear does not join latest durable surveillance truth");
    prior_latch = surveillance.value().fault_latch;
    expected_status.surveillance = surveillance.value().checkpoint;
    expected_status.surveillance_projection = surveillance.value().checkpoint.checkpoint_projection;
  }
  Require(recovery_intent.expected_latch_id == prior_latch.latch_id &&
              recovery_intent.expected_latch_generation == prior_latch.latch_generation &&
              recovery_intent.expected_recovery_assessment_revision >=
                  prior_latch.recovery_assessment_revision &&
              recovery_intent.expected_cell_id == prior_latch.cell_id &&
              recovery_intent.expected_retained_bundle_id == prior_latch.retained_bundle_id &&
              recovery_intent.expected_graph_revision == prior_latch.graph_revision &&
              prior_latch.state != StopFaultLatchStateDTO::kCleared &&
              recovery_intent.expected_recovery_assessment_revision <
                  std::numeric_limits<std::uint64_t>::max(),
          "operation_journal_stop_projection_conflict",
          "Stop fault clear expectations do not match latest durable latch truth");
  auto expected_cleared_latch = prior_latch;
  expected_cleared_latch.recovery_assessment_revision =
      recovery_intent.expected_recovery_assessment_revision + 1U;
  expected_cleared_latch.state = StopFaultLatchStateDTO::kCleared;
  expected_cleared_latch.recovery_status = StopFaultRecoveryStatusDTO::kCleared;
  expected_cleared_latch.reason_code = "stop_fault_recovery_cleared";
  expected_cleared_latch.evidence_refs = cleared_latch.evidence_refs;
  expected_cleared_latch.clear_projection = StopDurableProjectionStatusDTO{
      StopDurableProjectionStateDTO::kRecorded, std::nullopt, std::nullopt};
  expected_status.fault_latch = expected_cleared_latch;
  Require(cleared_latch == expected_cleared_latch && resulting_stop_status == expected_status,
          "operation_journal_stop_projection_conflict",
          "Stop fault clear does not equal the exact durable successor state");
  const StopJournalFaultClearProjectionDTO clear_projection{
      recovery_intent, recovery_result, resulting_stop_status, terminal.value().certificate};
  auto encoded = EncodeStopJournalFaultClearProjection(clear_projection);
  Require(encoded.ok(), "operation_journal_record_invalid",
          "Stop fault-clear durable payload is invalid");
  if (!current.IsNull(2)) {
    const auto sequence = PositiveIdentifier(current.Integer(2), "operation journal sequence");
    Require(current.Text(3) == recovery_result.request_id.value() &&
                current.Text(4) == encoded.value() && !current.Step(),
            "operation_journal_stop_projection_conflict",
            "Stop fault-clear identity conflicts with durable clear truth");
    transaction.Commit();
    return sequence;
  }
  Require(!current.Step(), "operation_journal_corrupt",
          "Stop fault-clear lookup returned duplicate rows");
  const auto payload_digest = content_identity::sha256_digest(encoded.value());
  Statement insert(database_,
                   "INSERT INTO journal_records(owner_epoch,record_kind,request_id,stop_id,"
                   "stop_generation,operation_id,operation_revision,occurred_at_ns,"
                   "occurred_at_wall,effect_pending,payload_json,payload_digest) "
                   "VALUES(?,'stop_fault_cleared',?,?,?,NULL,NULL,?,?,?,?,?)");
  insert.Bind(1, owner_epoch.value());
  insert.Bind(2, recovery_result.request_id.value());
  insert.Bind(3, cleared_latch.stop_id.value());
  insert.Bind(4, static_cast<std::int64_t>(cleared_latch.stop_generation.value()));
  insert.Bind(5, resulting_stop_status.accepted.accepted_at_ns);
  insert.Bind(6, occurred_at_wall);
  insert.Bind(7, static_cast<std::int64_t>(0));
  insert.Bind(8, encoded.value());
  insert.Bind(9, payload_digest);
  Require(!insert.Step(), "operation_journal_corrupt",
          "Stop fault-clear journal insert returned an unexpected row");
  const auto sequence =
      PositiveIdentifier(sqlite3_last_insert_rowid(database_), "operation journal sequence");
  Statement update(database_,
                   "UPDATE stop_projections SET clear_sequence=?,clear_request_id=?,"
                   "clear_payload=?,clear_digest=? WHERE stop_id=? AND stop_generation=? "
                   "AND clear_sequence IS NULL");
  update.Bind(1, static_cast<std::int64_t>(sequence));
  update.Bind(2, recovery_result.request_id.value());
  update.Bind(3, encoded.value());
  update.Bind(4, payload_digest);
  update.Bind(5, cleared_latch.stop_id.value());
  update.Bind(6, static_cast<std::int64_t>(cleared_latch.stop_generation.value()));
  Require(!update.Step() && sqlite3_changes(database_) == 1, "operation_journal_corrupt",
          "Stop fault-clear projection update did not affect exactly one row");
  transaction.Commit();
  return sequence;
}

std::uint64_t SqliteOperationJournal::Impl::CloseOwnerEpoch(const RequestOwnerEpoch& owner_epoch,
                                                            const std::int64_t closed_at_ns) {
  Require(!owner_epoch.empty() && closed_at_ns >= 0 &&
              static_cast<std::uint64_t>(closed_at_ns) <= kMaximumSafeInteger,
          "operation_journal_owner_epoch_invalid", "owner close identity is invalid");
  std::lock_guard lock(mutex_);
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
  Statement stop_blockers(database_,
                          "SELECT terminal_payload,surveillance_payload,clear_payload "
                          "FROM stop_projections");
  while (stop_blockers.Step()) {
    Require(!stop_blockers.IsNull(0), "operation_journal_owner_close_blocked",
            "owner close is blocked by a nonterminal Stop");
    auto terminal = DecodeStopJournalTerminalProjection(stop_blockers.Text(0));
    Require(terminal.ok(), "operation_journal_corrupt",
            "owner close cannot decode terminal Stop truth");
    const bool needs_clear = terminal.value().fault_latch.has_value() || !stop_blockers.IsNull(1);
    Require(!needs_clear || !stop_blockers.IsNull(2), "operation_journal_owner_close_blocked",
            "owner close is blocked by an active Stop fault or surveillance owner");
  }
  const auto payload = content_identity::canonical_json(CanonicalValue(CanonicalValue::Object{
      {"closed_at_ns", CanonicalValue(static_cast<double>(closed_at_ns))},
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
              request.expected_operation_revision == accepted.operation_revision &&
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
    const RuntimeScaleStatusDTO* scale_status, const char* transition_reason) {
  Require(transition_reason != nullptr && transition_reason[0] != '\0' &&
              (scale_status == nullptr ||
               (scale_status->operation_id == state.operation_id &&
                scale_status->operation_revision == state.operation_revision &&
                state.runtime_scale_generation == scale_status->runtime_scale_generation)),
          "operation_journal_record_invalid",
          "operation state observation is missing exact transition evidence");
  return Append(owner_epoch, "state_observed", std::nullopt,
                RecordValue("state_observed", owner_epoch,
                            CanonicalValue(CanonicalValue::Object{
                                {"transition_reason", CanonicalValue(transition_reason)},
                            }),
                            CanonicalValue(nullptr), state, scale_status, false),
                state, state.updated_at_ns, false, "current");
}

SqliteOperationJournal::SqliteOperationJournal(std::filesystem::path database_path, CellId cell_id,
                                               std::size_t history_capacity,
                                               std::function<std::string()> wall_time_text)
    : impl_(std::make_unique<Impl>(std::move(database_path), std::move(cell_id), history_capacity,
                                   std::move(wall_time_text))) {}

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
    const OperationAcceptedDTO& accepted, const OperationStateDTO& state) {
  return AsResult<std::uint64_t>(
      [&] { return impl_->RecordOperationAccepted(owner_epoch, intent, accepted, state); });
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
    const StopCertificateDTO& certificate,
    const std::optional<StopFaultLatchDTO>& fault_latch) noexcept {
  return AsStopForensicProjectionResult([&] {
    return impl_->RecordStopTerminal(owner_epoch, stop_status, certificate, fault_latch);
  });
}

StopForensicProjectionResultDTO SqliteOperationJournal::RecordStopSurveillanceCheckpoint(
    const RequestOwnerEpoch& owner_epoch, const StopPostTerminalSurveillanceDTO& checkpoint,
    const StopFaultLatchDTO& fault_latch, const std::int64_t projected_at_ns) noexcept {
  return AsStopForensicProjectionResult([&] {
    return impl_->RecordStopSurveillanceCheckpoint(owner_epoch, checkpoint, fault_latch,
                                                   projected_at_ns);
  });
}

Result<std::uint64_t> SqliteOperationJournal::RecordStopFaultCleared(
    const RequestOwnerEpoch& owner_epoch, const StopFaultRecoveryIntentDTO& recovery_intent,
    const StopFaultRecoveryResultDTO& recovery_result, const StopFaultLatchDTO& cleared_latch,
    const StopStatusDTO& resulting_stop_status) {
  return AsResult<std::uint64_t>([&] {
    return impl_->RecordStopFaultCleared(owner_epoch, recovery_intent, recovery_result,
                                         cleared_latch, resulting_stop_status);
  });
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
    const RuntimeScaleStatusDTO* scale_status, const char* transition_reason) {
  return AsResult<std::uint64_t>([&] {
    return impl_->RecordOperationState(owner_epoch, state, scale_status, transition_reason);
  });
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
