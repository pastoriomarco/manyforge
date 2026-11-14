#pragma once

// Private to the logical-scene owner sources; not installed.

#include "manyforge_scene/logical_scene_owner.hpp"

#include <fcntl.h>
#include <sqlite3.h>
#include <sys/stat.h>
#include <unistd.h>

#include <manyforge_content_identity/content_identity.hpp>
#include <manyforge_core/geometry/pose_math.hpp>
#include <manyforge_core/logical_scene_identity.hpp>
#include <manyforge_core/logical_scene_projection.hpp>
#include <manyforge_core/types/native_projection_capability.hpp>
#include <manyforge_core/types/typed_error.hpp>
#include <manyforge_deployment/canonical_empty_admission.hpp>
#include <manyforge_deployment/verified_retained_bundle.hpp>
#include <manyforge_persistence/sqlite_read_only_preflight.hpp>
#include <manyforge_persistence/sqlite_record_writer.hpp>

#include "embedded_canonical_empty_scene_schema.hpp"
#include "logical_scene_codec_internal.hpp"
#include "sqlite_scene_lifecycle_internal.hpp"

#if SQLITE_VERSION_NUMBER < 3037000
#error "canonical-empty scene owner requires SQLite 3.37.0 or newer"
#endif

#ifndef SQLITE_OPEN_NOFOLLOW
#error "canonical-empty scene owner requires SQLITE_OPEN_NOFOLLOW"
#endif

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <map>
#include <mutex>
#include <new>
#include <set>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>
#include <variant>

namespace manyforge::scene::logical_scene_owner_detail {

using core::ActiveBundlePublicationGeneration;

using core::ActiveBundlePublicationReceiptDTO;

using core::ActiveBundlePublicationRequestDTO;

using core::ApplyGeneration;

using core::ApplyId;

using core::BootstrapGeneration;

using core::BootstrapId;

using core::BootstrapProjectionSetSourceDTO;

using core::BootstrapPublicationTransactionDTO;

using core::CellId;

using core::CollisionPolicySnapshot;

using core::ContentDigest;

using core::ErrorDTO;

using core::GraphRevision;

using core::LogicalSceneBootstrapIdentityInputDTO;

using core::LogicalSceneSnapshotDTO;

using core::LogicalSceneSnapshotRoot;

using core::MaintenanceProjectionSetSourceDTO;

using core::NamedApplyPublicationTransactionDTO;

using core::ProjectionAggregateHealthDTO;

using core::ProjectionBootstrapAcceptedDTO;

using core::ProjectionBootstrapEvidenceDTO;

using core::ProjectionBootstrapFinalizeDTO;

using core::ProjectionBootstrapFinalizeResultDTO;

using core::ProjectionBootstrapFinalizeResultState;

using core::ProjectionBootstrapIntentDTO;

using core::ProjectionBootstrapPrerequisiteQueryDTO;

using core::ProjectionBootstrapPrerequisiteState;

using core::ProjectionBootstrapPrerequisiteStatusDTO;

using core::ProjectionBootstrapRebuildRequestDTO;

using core::ProjectionBootstrapRebuildResultDTO;

using core::ProjectionBootstrapRebuildResultState;

using core::ProjectionBootstrapStatusDTO;

using core::ProjectionBootstrapStatusQueryDTO;

using core::ProjectionBootstrapStatusState;

using core::ProjectionHealthState;

using core::ProjectionMaintenanceCompletionDTO;

using core::ProjectionMaintenanceInitializationQueryDTO;

using core::ProjectionMaintenanceInitializationRequestDTO;

using core::ProjectionMaintenanceInitializationState;

using core::ProjectionMaintenanceInitializationStatusDTO;

using core::ProjectionRevisionDTO;

using core::ProjectionStageGeneration;

using core::RequestKey;

using core::RetainedBundleDTO;

using core::RetainedBundleId;

using core::SceneCommitResultDTO;

using core::SceneId;

using core::SceneObjectSnapshotDTO;

using core::SceneObjectStateDTO;

using core::SceneProjectionCommitStatusDTO;

using core::SceneResourceId;

using core::SceneRevision;

using core::SceneTransactionRequestDTO;

using core::SceneUpdateStatusDTO;

constexpr int kBusyTimeoutMilliseconds = 10000;

constexpr std::int64_t kLogicalSceneTransactionStoreSchemaVersion = 4;

constexpr std::int64_t kBoundedHistoryStoreSchemaVersion = 7;

constexpr std::int64_t kMaintenanceInitializationSchemaVersion = 8;

constexpr std::int64_t kInitialSceneBaselineSchemaVersion = 9;

constexpr std::int64_t kNativeProjectionHandoffStoreSchemaVersion = 5;

constexpr std::uint64_t kMaximumSafeIntegral = 9007199254740991ULL;

// One owner event per resident Scene transaction intent.
constexpr std::size_t kMaximumLogicalSceneOwnerEvents =
    kMaximumLogicalSceneTransactions;

class CellAdmissionTemporarilyUnavailable final : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

[[noreturn]] void ThrowCellAdmissionFailure(
    const std::string_view context, const core::ErrorDTO &error);

[[noreturn]] void ThrowSqlite(sqlite3 *database, const int result,
                              const std::string_view context);

void Check(sqlite3 *database, const int result,
           const std::string_view context);

void Exec(sqlite3 *database, const std::string_view sql);

class PersistentStatementCache final {
public:
  ~PersistentStatementCache() { Clear(); }
  PersistentStatementCache() = default;
  PersistentStatementCache(const PersistentStatementCache &) = delete;
  PersistentStatementCache &operator=(const PersistentStatementCache &) = delete;

  [[nodiscard]] sqlite3_stmt *Acquire(sqlite3 *database,
                                      const std::string_view sql) {
    const auto found = statements_.find(sql);
    if (found != statements_.end()) {
      return found->second;
    }
    sqlite3_stmt *statement = nullptr;
    Check(database,
          sqlite3_prepare_v3(database, std::string(sql).c_str(), -1,
                             SQLITE_PREPARE_PERSISTENT, &statement, nullptr),
          "prepare SQLite statement");
    try {
      statements_.emplace(std::string(sql), statement);
    } catch (...) {
      sqlite3_finalize(statement);
      throw;
    }
    return statement;
  }

  void Clear() noexcept {
    for (const auto &[sql, statement] : statements_) {
      static_cast<void>(sql);
      sqlite3_finalize(statement);
    }
    statements_.clear();
  }

private:
  std::map<std::string, sqlite3_stmt *, std::less<>> statements_;
};

class Statement final {
public:
  Statement(sqlite3 *database, const std::string_view sql,
            PersistentStatementCache *cache = nullptr)
      : database_(database), cached_(cache != nullptr) {
    if (cache != nullptr) {
      statement_ = cache->Acquire(database, sql);
    } else {
      Check(database_,
            sqlite3_prepare_v2(database_, std::string(sql).c_str(), -1,
                               &statement_, nullptr),
            "prepare SQLite statement");
    }
  }

  ~Statement() {
    if (statement_ != nullptr) {
      if (cached_) {
        // Like the previous finalize, cleanup does not replace an earlier
        // step/validation error with a cleanup error.
        sqlite3_reset(statement_);
        sqlite3_clear_bindings(statement_);
      } else {
        sqlite3_finalize(statement_);
      }
    }
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

  /// A request key is stored as its 16 bytes.
  void Bind(const int index, const RequestKey &key) {
    Check(database_,
          sqlite3_bind_blob(statement_, index, key.bytes.data(),
                            static_cast<int>(key.bytes.size()),
                            SQLITE_TRANSIENT),
          "bind SQLite request key");
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
      throw LogicalSceneOwnerIntegrityError(
          "stored logical-scene text has an invalid SQLite type");
    }
    const auto *value = sqlite3_column_text(statement_, column);
    if (value == nullptr) {
      throw LogicalSceneOwnerIntegrityError(
          "stored logical-scene text is null");
    }
    return {
        reinterpret_cast<const char *>(value),
        static_cast<std::size_t>(sqlite3_column_bytes(statement_, column)),
    };
  }

  [[nodiscard]] std::int64_t Integer(const int column) const {
    if (sqlite3_column_type(statement_, column) != SQLITE_INTEGER) {
      throw LogicalSceneOwnerIntegrityError(
          "stored logical-scene integer has an invalid SQLite type");
    }
    return sqlite3_column_int64(statement_, column);
  }

  [[nodiscard]] RequestKey Key(const int column) const {
    RequestKey key;
    if (sqlite3_column_type(statement_, column) != SQLITE_BLOB ||
        sqlite3_column_bytes(statement_, column) !=
            static_cast<int>(key.bytes.size())) {
      throw LogicalSceneOwnerIntegrityError(
          "stored logical-scene request key is not 16 bytes");
    }
    const auto *bytes = static_cast<const std::uint8_t *>(
        sqlite3_column_blob(statement_, column));
    std::copy(bytes, bytes + key.bytes.size(), key.bytes.begin());
    return key;
  }

  [[nodiscard]] bool IsNull(const int column) const noexcept {
    return sqlite3_column_type(statement_, column) == SQLITE_NULL;
  }

  [[nodiscard]] int Columns() const noexcept {
    return sqlite3_column_count(statement_);
  }

  [[nodiscard]] int Type(const int column) const noexcept {
    return sqlite3_column_type(statement_, column);
  }

  /// The stored length of a text or blob column.
  [[nodiscard]] std::size_t Bytes(const int column) const noexcept {
    return static_cast<std::size_t>(sqlite3_column_bytes(statement_, column));
  }

private:
  sqlite3 *database_;
  bool cached_;
  sqlite3_stmt *statement_{};
};

// Cached values are keyed by retained sequence and column, and are usable only
// after the exact SQLite text bytes have been fetched and compared again.
class MaintenanceSnapshotCache final {
public:
  struct Entry final {
    std::int64_t sequence;
    std::string bytes;
    LogicalSceneSnapshotDTO snapshot;
    std::optional<ContentDigest> content_hash;
  };

  [[nodiscard]] Entry *Find(const std::int64_t sequence, const bool baseline,
                            const std::string_view bytes) noexcept {
    auto &entry = entries_[Index(sequence, baseline)];
    if (!entry.has_value() || entry->sequence != sequence ||
        entry->bytes != bytes) {
      return nullptr;
    }
    return &*entry;
  }

  void Store(const std::int64_t sequence, const bool baseline,
             std::string bytes, const LogicalSceneSnapshotDTO &snapshot) {
    entries_[Index(sequence, baseline)] =
        Entry{sequence, std::move(bytes), snapshot, std::nullopt};
  }

  void Clear() noexcept {
    for (auto &entry : entries_) {
      entry.reset();
    }
  }

private:
  static std::size_t Index(const std::int64_t sequence,
                           const bool baseline) noexcept {
    return (static_cast<std::uint64_t>(sequence) %
            kMaximumMaintenanceInitializations) * 2U + (baseline ? 1U : 0U);
  }
  std::array<std::optional<Entry>, kMaximumMaintenanceInitializations * 2U>
      entries_{};
};

class Transaction final {
public:
  Transaction(sqlite3 *database, const std::string_view begin,
              MaintenanceSnapshotCache *cache = nullptr)
      : database_(database), cache_(cache) {
    Exec(database_, begin);
  }

  ~Transaction() {
    if (!finished_) {
      sqlite3_exec(database_, "ROLLBACK", nullptr, nullptr, nullptr);
      if (cache_ != nullptr) {
        cache_->Clear();
      }
    }
  }

  Transaction(const Transaction &) = delete;
  Transaction &operator=(const Transaction &) = delete;

  void Commit() {
    Exec(database_, "COMMIT");
    finished_ = true;
  }

private:
  sqlite3 *database_;
  MaintenanceSnapshotCache *cache_;
  bool finished_{};
};

std::int64_t ScalarInteger(sqlite3 *database, const std::string_view sql,
                           PersistentStatementCache *cache = nullptr);

void ValidateBoundedIdentity(const std::string_view value,
                             const std::string_view label);

std::int64_t CheckedSafeCounter(const std::uint64_t value,
                                const std::string_view label);

void ValidateFixedQuery(const ProjectionBootstrapPrerequisiteQueryDTO &query);

struct AdmittedBootstrapAuthority final {
  ProjectionBootstrapPrerequisiteQueryDTO query;
  ContentDigest admission_digest;
  RetainedBundleDTO retained_bundle;
};

AdmittedBootstrapAuthority AuthorityFromAdmission(
    const deployment::CanonicalEmptyRetainedBundleAdmission &admission);

struct AcceptedState final {
  BootstrapId bootstrap_id;
  BootstrapGeneration bootstrap_generation;
  ProjectionRevisionDTO target;
  LogicalSceneSnapshotDTO snapshot;
};

struct MaintenanceInitializationState final {
  std::uint64_t initialization_sequence{};
  ApplyId apply_id;
  ApplyGeneration apply_generation;
  ProjectionRevisionDTO predecessor;
  ProjectionRevisionDTO target;
  LogicalSceneSnapshotDTO authoritative_snapshot;
  std::vector<core::ProjectionExecutionHostBindingDTO> bindings;
  std::optional<ProjectionMaintenanceInitializationState> result_state;
  std::optional<ErrorDTO> first_error;
  bool effects_unknown{};
  std::optional<ActiveBundlePublicationReceiptDTO> publication_receipt;
  std::optional<std::uint64_t> repaired_transaction_sequence;
  std::optional<ContentDigest> initial_baseline_request_digest;
  bool initial_baseline_installed{true};
  std::optional<ErrorDTO> initial_baseline_error;
  bool initial_baseline_effects_unknown{};
  // A baseline intent row exists for this initialization.
  bool initial_baseline_intent_recorded{};
};

struct DurableState final {
  std::optional<AcceptedState> accepted;
  bool rebuilt{};
  std::optional<ActiveBundlePublicationReceiptDTO> finalization_receipt;
  std::optional<ProjectionRevisionDTO> active_projection;
  std::optional<LogicalSceneSnapshotDTO> active_scene_snapshot;
  std::vector<MaintenanceInitializationState> maintenance_initializations;
  bool projection_desynchronized{};
  bool unresolved_scene_transaction{};
  std::optional<std::uint64_t> latest_uncertain_transaction_sequence;

  [[nodiscard]] const MaintenanceInitializationState *
  LatestMaintenanceInitialization() const noexcept {
    return maintenance_initializations.empty()
               ? nullptr
               : &maintenance_initializations.back();
  }

  [[nodiscard]] CanonicalEmptySceneBootstrapPhase phase() const noexcept {
    if (finalization_receipt.has_value()) {
      return CanonicalEmptySceneBootstrapPhase::kFinalized;
    }
    if (rebuilt) {
      return CanonicalEmptySceneBootstrapPhase::kReadyToPublish;
    }
    if (accepted.has_value()) {
      return CanonicalEmptySceneBootstrapPhase::kAccepted;
    }
    return CanonicalEmptySceneBootstrapPhase::kInitialCommandFenced;
  }
};

[[nodiscard]] CanonicalEmptySceneAccessStatus
AccessStatus(const DurableState &state);

/// Returns the exact Scene-owned current native binding set: the one of the
/// latest published maintenance completion, else none.
[[nodiscard]] std::vector<core::ProjectionExecutionHostBindingDTO>
ActiveExecutionHostBindings(const DurableState &state);

struct StoredSceneOutcome final {
  RequestKey request_key;
  ContentDigest request_digest;
  std::optional<std::uint64_t> transaction_sequence;
  bool success;
  SceneProjectionCommitStatusDTO projection_status;
  std::size_t attempted_projection_count;
  std::optional<ErrorDTO> error;
};

SceneProjectionCommitStatusDTO
DecodeProjectionStatus(std::string_view value);

[[nodiscard]] bool IsAuthoredSceneMutation(
    const SceneTransactionRequestDTO &request);

/// Whether a request's expected projection admits the active one. The Scene
/// revision is never a precondition: any revision of the same active
/// Deployment (bundle and graph) and Scene id is accepted, and the operation
/// applies to the latest Scene (last arrival wins). Operations still check
/// their own identities (object, attachment, carrier).
[[nodiscard]] bool ExpectedProjectionAdmitsActive(
    const SceneTransactionRequestDTO &request,
    const ProjectionRevisionDTO &active);

[[nodiscard]] core::CellAdmissionClaimIntentDTO AuthoredSceneClaimIntent(
    const SceneTransactionRequestDTO &request);

void ReleaseAuthoredSceneClaim(
    core::IKernelCellAdmissionPort &admission,
    const core::CellAdmissionClaimDTO &claim,
    const ContentDigest &terminal_evidence,
    const core::CellAdmissionSettlementKindDTO settlement_kind);

void SettleAuthoredSceneClaimIfHeld(
    core::IKernelCellAdmissionPort &admission,
    const SceneTransactionRequestDTO &request,
    const ContentDigest &request_digest, const bool success,
    const SceneProjectionCommitStatusDTO projection_status);

struct SceneStoreMemory;

void SettleRecoveredAuthoredSceneClaims(
    const SceneStoreMemory &memory, core::IKernelCellAdmissionPort &admission);

struct SceneTransactionStoreState final {
  ProjectionRevisionDTO active_projection;
  LogicalSceneSnapshotDTO active_snapshot;
  std::uint64_t next_transaction_sequence;
  std::uint64_t next_owner_event_sequence;
  std::size_t resident_transaction_count;
  bool projection_desynchronized;
  bool unresolved_native_attempt;
  std::optional<std::uint64_t> unresolved_transaction_sequence;
  std::optional<std::uint64_t> latest_uncertain_transaction_sequence;
};

/// The replayed Scene state for the ordinary commit path, derived from
/// `SceneStoreMemory`. Commit keeps it current; every other mutation drops it
/// and the next read replays the memory rows again.
struct SceneCommitStateCache final {
  DurableState durable_state;
  std::optional<SceneTransactionStoreState> transaction_state;
};

struct BoundedHistoryState final {
  std::uint64_t compaction_generation{};
  std::uint64_t compacted_through_transaction_sequence{};
  std::uint64_t compacted_through_owner_event_sequence{};
  std::size_t compacted_maintenance_completion_count{};
  std::uint64_t recovered_through_transaction_sequence{};
  std::optional<ProjectionRevisionDTO> checkpoint_projection;
  std::optional<LogicalSceneSnapshotDTO> checkpoint_snapshot;
};

/// One resident Scene transaction: its intent row, the count of its attempt
/// rows and the states of its result rows, in ordinal order.
struct SceneTransactionRecord final {
  std::uint64_t transaction_sequence;
  RequestKey request_key;
  ContentDigest request_digest;
  std::string request_json;
  ProjectionRevisionDTO prior;
  ProjectionRevisionDTO proposed;
  std::string candidate_snapshot_json;
  SceneUpdateStatusDTO update_status;
  std::size_t required_projection_count;
  std::size_t projection_attempt_count;
  std::vector<core::ProjectionCommitResultStateDTO> projection_results;
};

struct SceneOwnerEventRecord final {
  std::uint64_t owner_event_sequence;
  std::uint64_t transaction_sequence;
};

/// The store's rows, held in memory. This is the authority every read and
/// every write decision uses; the file receives the same rows afterwards.
struct SceneStoreMemory final {
  std::optional<AcceptedState> accepted;
  bool rebuilt{};
  std::optional<ActiveBundlePublicationReceiptDTO> finalization_receipt;
  std::vector<MaintenanceInitializationState> maintenance_initializations;
  BoundedHistoryState history;
  std::vector<SceneTransactionRecord> transactions;
  std::map<RequestKey, StoredSceneOutcome> outcomes;
  std::vector<SceneOwnerEventRecord> owner_events;
  // The file bytes of the rows (SceneRowFileBytes, od212): of every Scene
  // transaction row, and of each maintenance initialization's rows by
  // initialization sequence.
  std::uint64_t transaction_file_bytes{};
  std::map<std::uint64_t, std::uint64_t> maintenance_file_bytes;
};

/// Start only: reads the rows of an already validated store.
[[nodiscard]] SceneStoreMemory LoadSceneStoreMemory(
    sqlite3 *database,
    const ProjectionBootstrapPrerequisiteQueryDTO &fixed_query);

[[nodiscard]] BoundedHistoryState ReadBoundedHistoryState(sqlite3 *database);

SceneProjectionCommitStatusDTO
DecodeProjectionStatus(const std::string_view value);

std::optional<StoredSceneOutcome>
ReadOutcomeForRequest(sqlite3 *database, const RequestKey &request_key);

std::optional<StoredSceneOutcome>
ReadOutcomeForRequest(const SceneStoreMemory &memory,
                      const RequestKey &request_key);

SceneTransactionStoreState
ReadSceneTransactionStoreState(sqlite3 *database,
                               const DurableState &durable_state);

SceneTransactionStoreState
ReadSceneTransactionStoreState(const SceneStoreMemory &memory,
                               const DurableState &durable_state);

struct PreparedSceneHistoryCompaction final {
  BoundedHistoryState history;
  std::string checkpoint_snapshot_json;
};

/// Checks a compaction of the resident Scene ledger behind a checkpoint of
/// the settled head. `maintenance_fold` admits an empty ledger and a closed
/// maintenance window.
[[nodiscard]] PreparedSceneHistoryCompaction PrepareSceneHistoryCompaction(
    const SceneStoreMemory &memory, const DurableState &durable_state,
    const SceneTransactionStoreState &transactions, bool maintenance_fold);

/// Applies a prepared compaction to memory and appends its statements.
void CompactSceneTransactionHistory(
    SceneStoreMemory &memory, persistence::SqliteRecord &record,
    PreparedSceneHistoryCompaction compaction,
    const DurableState &durable_state,
    const SceneTransactionStoreState &transactions);

ErrorDTO SceneError(const std::string_view code, const std::string_view message,
                    std::map<std::string, std::string> details = {});

SceneCommitResultDTO
FailedSceneResult(const RequestKey &request_key,
                  const core::RequestReplayStatusDTO replay, ErrorDTO error,
                  const SceneProjectionCommitStatusDTO projection_status);

bool SamePose(const core::Pose3DTO &left,
              const core::Pose3DTO &right) noexcept;

class SceneRequestRejectionError final : public core::TypedError {
public:
  SceneRequestRejectionError(std::string code, std::string message,
                             std::map<std::string, std::string> details = {})
      : core::TypedError(std::move(code), message, std::move(details)) {}
};

std::vector<core::SupportContactDTO>
NormalizeSupportContacts(const core::SceneResourceId &scene_resource_id,
                         std::vector<core::SupportContactDTO> support_contacts,
                         const core::LogicalSceneSnapshotDTO &current);

struct NormalizedScenePose final {
  core::Pose3DTO pose_in_cell;
  ContentDigest resolution_evidence_digest;
};

[[nodiscard]] NormalizedScenePose
NormalizeScenePose(const core::SceneTransactionFrameRefDTO &frame_ref,
                   const core::Pose3DTO &pose_in_frame,
                   const LogicalSceneSnapshotDTO &current,
                   const ProjectionRevisionDTO &expected_projection,
                   const ContentDigest &request_digest,
                   const std::optional<core::SceneFrameResolutionReceiptDTO>
                       &frame_resolution);

/// Records one outcome row in memory and appends its insert.
void InsertSceneOutcome(SceneStoreMemory &memory,
                        persistence::SqliteRecord &record,
                        const RequestKey &request_key,
                        const ContentDigest &request_digest,
                        const std::optional<std::uint64_t> transaction_sequence,
                        const bool success,
                        const SceneProjectionCommitStatusDTO projection_status,
                        const std::size_t attempted_projection_count,
                        const std::optional<ErrorDTO> &error);

SceneCommitResultDTO
ResultFromStoredOutcome(const SceneStoreMemory &memory,
                        const StoredSceneOutcome &outcome,
                        const core::RequestReplayStatusDTO replay_status);

/// Binds a request key as its 16 bytes.
void BindRequestKey(persistence::SqliteRecord &record, int index,
                    const RequestKey &key);

struct RequiredSceneProjection final {
  std::shared_ptr<core::IPlannerSceneProjection> projection;
  core::ProjectionHealthDTO preflight_health;
};

[[nodiscard]] RequiredSceneProjection ResolveRequiredProjection(
    const core::ProjectionExecutionHostBindingDTO &binding,
    const core::IPlannerSceneProjectionResolver *resolver,
    const ProjectionRevisionDTO &selected_revision);

[[nodiscard]] std::optional<core::SceneFrameResolutionReceiptDTO>
ResolveFrameAcrossProjections(
    const core::SceneFrameResolutionSelectorDTO &selector,
    const ProjectionRevisionDTO &projection_revision,
    const std::vector<RequiredSceneProjection> &projections,
    const std::string_view lease_scope);

[[nodiscard]] core::CollisionPolicySnapshot ProjectEffectivePolicy(
    const LogicalSceneSnapshotDTO &candidate,
    const std::vector<RequiredSceneProjection> &projections);

DurableState ReadCompleteDurableState(const SceneStoreMemory &memory);

/// Refuses any store whose application id or schema version is not the
/// current one; an older store is never converted.
void ValidateDatabase(
    sqlite3 *database,
    const ProjectionBootstrapPrerequisiteQueryDTO &fixed_query,
    const ContentDigest &admission_digest);

void InsertMetadata(sqlite3 *database,
                    const ProjectionBootstrapPrerequisiteQueryDTO &fixed_query,
                    const ContentDigest &admission_digest);

void InsertMigrationMetadata(sqlite3 *database);

void InsertNativeHandoffMigrationMetadata(sqlite3 *database);

void InsertBoundedHistoryMigrationMetadata(sqlite3 *database);

void InsertMaintenanceInitializationMigrationMetadata(sqlite3 *database);

void InsertInitialSceneBaselineMigrationMetadata(sqlite3 *database);

void ConfigureConnection(sqlite3 *database);

void ConfigureDurability(sqlite3 *database);

bool IsExactUninitializedDatabase(sqlite3 *database);

void FsyncDatabaseDirectory(const std::filesystem::path &database_path);

CanonicalEmptySceneAccessStatus AccessStatus(const DurableState &state);

// The store's file bytes (od212) ---------------------------------------------

/// The size of one row's SQLite record (file format 2.1): a header of serial
/// types, then the values.
class RecordSize final {
public:
  void Integer(const std::int64_t value) {
    const auto magnitude =
        static_cast<std::uint64_t>(value < 0 ? ~value : value);
    std::uint64_t size = 8U;
    if (value == 0 || value == 1) {
      size = 0U;
    } else if (magnitude <= 0x7FU) {
      size = 1U;
    } else if (magnitude <= 0x7FFFU) {
      size = 2U;
    } else if (magnitude <= 0x7FFFFFU) {
      size = 3U;
    } else if (magnitude <= 0x7FFFFFFFU) {
      size = 4U;
    } else if (magnitude <= 0x7FFFFFFFFFFFU) {
      size = 6U;
    }
    header_ += 1U;
    body_ += size;
  }
  /// A text or blob value of `bytes` bytes.
  void Text(const std::size_t bytes) {
    header_ += VarintLength(2U * bytes + 13U);
    body_ += bytes;
  }
  void Null() { header_ += 1U; }
  [[nodiscard]] std::uint64_t bytes() const {
    auto header = header_;
    if (header <= 126U) {
      header += 1U;
    } else {
      const auto length = VarintLength(header);
      header += length;
      if (length < VarintLength(header)) {
        ++header;
      }
    }
    return header + body_;
  }

private:
  static std::uint64_t VarintLength(std::uint64_t value) {
    std::uint64_t length = 1U;
    while (value > 0x7FU && length < 9U) {
      value >>= 7U;
      ++length;
    }
    return length;
  }
  std::uint64_t header_{};
  std::uint64_t body_{};
};

/// One inserted row: binds its values into `record` and sizes its record.
class SizedRow final {
public:
  SizedRow(persistence::SqliteRecord &record, std::string sql)
      : record_(record) {
    record_.Statement(std::move(sql));
  }
  void Bind(const int index, const std::int64_t value) {
    record_.Bind(index, value);
    size_.Integer(value);
  }
  void Bind(const int index, const std::string_view text) {
    record_.Bind(index, text);
    size_.Text(text.size());
  }
  void BindBlob(const int index, const std::string_view bytes) {
    record_.BindBlob(index, bytes);
    size_.Text(bytes.size());
  }
  void BindNull(const int index) {
    record_.BindNull(index);
    size_.Null();
  }
  /// A text column whose value the statement's SQL carries.
  void Literal(const std::string_view text) { size_.Text(text.size()); }
  [[nodiscard]] std::uint64_t record_bytes() const { return size_.bytes(); }

private:
  persistence::SqliteRecord &record_;
  RecordSize size_;
};

/// Binds a request key as its 16 bytes and sizes it.
void BindRequestKey(SizedRow &row, int index, const RequestKey &key);

/// A table of Scene transaction or maintenance-initialization rows: its
/// b-tree kind and its number of UNIQUE indexes. No other table grows.
struct SceneRowTable final {
  std::string_view name;
  bool rowid;
  std::uint64_t unique_indexes;
  bool maintenance;
};

inline constexpr std::array<SceneRowTable, 12> kSceneRowTables{{
    {"logical_scene_transaction_intents", true, 1U, false},
    {"logical_scene_owner_events", true, 1U, false},
    {"logical_scene_request_outcomes", false, 0U, false},
    {"logical_scene_projection_attempts", false, 0U, false},
    {"logical_scene_projection_results", false, 0U, false},
    {"logical_scene_maintenance_initialization_intents", true, 1U, true},
    {"logical_scene_maintenance_initialization_bindings", false, 0U, true},
    {"logical_scene_maintenance_initialization_results", true, 0U, true},
    {"logical_scene_maintenance_initialization_completions", true, 0U, true},
    {"logical_scene_initial_baseline_requests", true, 0U, true},
    {"logical_scene_initial_baseline_intents", true, 0U, true},
    {"logical_scene_initial_baseline_results", true, 0U, true},
}};

consteval const SceneRowTable &SceneTable(const std::string_view name) {
  for (const auto &table : kSceneRowTables) {
    if (table.name == name) {
      return table;
    }
  }
  throw "not a Scene row table";
}

/// The part of the page image the rows may take: the rest holds the schema
/// (59 pages with the root page of each of its 28 b-trees), the singleton
/// rows with the history checkpoint twice while it is replaced (at most 2 x
/// 34 pages), and at most 17 free-list trunk pages, with room to spare.
inline constexpr std::uint64_t kSceneStoreReservedFileBytes = 1024U * 1024U;
inline constexpr std::uint64_t kSceneStorePageBytes = 4096U;
inline constexpr std::uint64_t kSceneStoreRecordFileBytes =
    static_cast<std::uint64_t>(kMaximumSceneStoreDatabasePageImageBytes) -
    kSceneStoreReservedFileBytes;

/// The file bytes one row of `record` bytes and its UNIQUE index entries can
/// take on the store's 4096-byte pages (SQLite file format): an over-count.
///  - A cell of P bytes stays in its page when P <= X (index b-tree, which
///    holds WITHOUT ROWID rows: X = 1002; rowid table: X = 4061); otherwise
///    M = 489 to X bytes stay and the rest goes to at most
///    ceil((P-M)/4092) overflow pages. Counting M bytes and that many whole
///    pages counts one page more whenever more than M bytes stay.
///  - A cell takes at most 13 more bytes in an index b-tree (2-byte pointer,
///    3-byte size, child and overflow page numbers) and at most 33 in a
///    rowid table (pointer, size, 9-byte rowid, overflow page number, and
///    the 15-byte interior cell of its leaf page).
///  - The in-page bytes count three times: SQLite rebalances a page that is
///    less than one third full.
///  - An index entry (a request key, sequence or Apply identity with the
///    rowid) is at most 300 bytes.
[[nodiscard]] inline std::uint64_t
SceneRowFileBytes(const std::uint64_t record, const SceneRowTable &table) {
  constexpr std::uint64_t kPageFill = 3U;
  constexpr std::uint64_t kIndexCellOverhead = 13U;
  constexpr std::uint64_t kMaximumIndexEntryBytes = 300U;
  constexpr auto kUsable = kSceneStorePageBytes;
  const auto overhead = table.rowid ? 33U : kIndexCellOverhead;
  const auto in_page = table.rowid ? kUsable - 35U
                                   : (kUsable - 12U) * 64U / 255U - 23U;
  const auto minimum_in_page = (kUsable - 12U) * 32U / 255U - 23U;
  const auto per_overflow_page = kUsable - 4U;
  const auto row =
      record <= in_page
          ? kPageFill * (record + overhead)
          : kPageFill * (minimum_in_page + overhead) +
                kUsable * ((record - minimum_in_page + per_overflow_page - 1U) /
                           per_overflow_page);
  return row + table.unique_indexes * kPageFill *
                   (kMaximumIndexEntryBytes + kIndexCellOverhead);
}

/// The file bytes every held row takes.
[[nodiscard]] std::uint64_t SceneFileBytes(const SceneStoreMemory &memory);

/// The initializations the eviction rule gives up, one after the other, so
/// that `bytes` more fit the store's file bytes once `freed` bytes are given
/// up, with `folded` completions folded into the history checkpoint; also
/// at least one when `one` (the count limit). Changes nothing; the rows are
/// `state`'s.
[[nodiscard]] std::vector<const MaintenanceInitializationState *>
MaintenanceEvictions(const SceneStoreMemory &memory, const DurableState &state,
                     std::size_t folded, std::uint64_t freed,
                     std::uint64_t bytes, bool one);

/// Deletes one initialization and its rows in memory and appends the deletes.
void DeleteMaintenanceInitialization(SceneStoreMemory &memory,
                                     persistence::SqliteRecord &record,
                                     const MaintenanceInitializationState &row);

[[nodiscard]] const SceneObjectSnapshotDTO *
FindSceneObject(const LogicalSceneSnapshotDTO &snapshot,
                const core::SceneResourceId &scene_resource_id);

} // namespace manyforge::scene::logical_scene_owner_detail

namespace manyforge::scene {
using namespace logical_scene_owner_detail;
} // namespace manyforge::scene
