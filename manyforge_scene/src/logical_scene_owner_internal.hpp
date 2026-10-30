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
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
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

using core::RequestId;

using core::RequestOwnerEpoch;

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

constexpr std::size_t kMaximumLogicalSceneOwnerEvents =
    kMaximumLogicalSceneTransactions + kMaximumMigratedStructuralCompletions;

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

class Statement final {
public:
  Statement(sqlite3 *database, const std::string_view sql)
      : database_(database) {
    Check(database_,
          sqlite3_prepare_v2(database_, std::string(sql).c_str(), -1,
                             &statement_, nullptr),
          "prepare SQLite statement");
  }

  ~Statement() {
    if (statement_ != nullptr) {
      sqlite3_finalize(statement_);
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

  [[nodiscard]] bool IsNull(const int column) const noexcept {
    return sqlite3_column_type(statement_, column) == SQLITE_NULL;
  }

private:
  sqlite3 *database_;
  sqlite3_stmt *statement_{};
};

class Transaction final {
public:
  Transaction(sqlite3 *database, const std::string_view begin)
      : database_(database) {
    Exec(database_, begin);
  }

  ~Transaction() {
    if (!finished_) {
      sqlite3_exec(database_, "ROLLBACK", nullptr, nullptr, nullptr);
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
  bool finished_{};
};

std::int64_t ScalarInteger(sqlite3 *database, const std::string_view sql);

void ValidateBoundedIdentity(const std::string_view value,
                             const std::string_view label);

RequestOwnerEpoch
CompactedRequestOwnerEpoch(const SceneId &scene_id,
                           const std::uint64_t request_epoch_generation);

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

struct MigratedStructuralCompletionState final {
  std::uint64_t completion_sequence;
  ApplyId apply_id;
  ApplyGeneration apply_generation;
  ProjectionRevisionDTO target;
};

struct MaintenanceInitializationState final {
  std::uint64_t initialization_sequence{};
  ApplyId apply_id;
  ApplyGeneration apply_generation;
  ProjectionRevisionDTO predecessor;
  ProjectionRevisionDTO target;
  LogicalSceneSnapshotDTO authoritative_snapshot;
  std::vector<core::ProjectionNativeRuntimeBindingDTO> bindings;
  std::optional<ProjectionMaintenanceInitializationState> result_state;
  std::optional<ErrorDTO> first_error;
  bool effects_unknown{};
  std::optional<ActiveBundlePublicationReceiptDTO> publication_receipt;
  std::optional<std::uint64_t> repaired_transaction_sequence;
  std::optional<ContentDigest> initial_baseline_request_digest;
  bool initial_baseline_installed{true};
  std::optional<ErrorDTO> initial_baseline_error;
  bool initial_baseline_effects_unknown{};
};

struct DurableState final {
  std::optional<AcceptedState> accepted;
  bool rebuilt{};
  std::optional<ActiveBundlePublicationReceiptDTO> finalization_receipt;
  std::vector<MigratedStructuralCompletionState> migrated_structural_completions;
  std::optional<ProjectionRevisionDTO> active_projection;
  std::optional<LogicalSceneSnapshotDTO> active_scene_snapshot;
  std::vector<core::ProjectionNativeRuntimeBindingDTO>
      active_native_runtime_bindings;
  bool legacy_native_effects_uncertain{};
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

/// Returns the exact Scene-owned current native binding set. A maintenance
/// completion supplies the complete set; only a pre-UA4 store falls back to
/// its immutable migration snapshot.
[[nodiscard]] std::vector<core::ProjectionNativeRuntimeBindingDTO>
ActiveNativeRuntimeBindings(const DurableState &state);

struct StoredSceneOutcome final {
  std::string request_owner_epoch;
  RequestId request_id;
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
    const SceneTransactionRequestDTO &request,
    const ContentDigest &request_digest);

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

void SettleRecoveredAuthoredSceneClaims(
    sqlite3 *database, core::IKernelCellAdmissionPort &admission);

struct SceneTransactionStoreState final {
  ProjectionRevisionDTO active_projection;
  LogicalSceneSnapshotDTO active_snapshot;
  RequestOwnerEpoch request_owner_epoch;
  std::uint64_t next_transaction_sequence;
  std::uint64_t next_owner_event_sequence;
  std::size_t resident_transaction_count;
  bool projection_desynchronized;
  bool unresolved_native_attempt;
  std::optional<std::uint64_t> unresolved_transaction_sequence;
  std::optional<std::uint64_t> latest_uncertain_transaction_sequence;
};

/// Process-local acceleration for the ordinary scene-commit path.
///
/// Startup and every structural/recovery mutation still reconstruct and
/// validate the complete durable history. Between those boundaries this owner
/// is the sole writer and holds `Impl::mutex_`, so replaying all resident
/// transaction JSON before every edit is both redundant and quadratic. The
/// SQLite `data_version` value fences writes made through another connection;
/// any such change discards this cache and forces a complete reconstruction.
struct SceneCommitStateCache final {
  DurableState durable_state;
  std::optional<SceneTransactionStoreState> transaction_state;
  std::int64_t external_data_version{};
};

struct BoundedHistoryState final {
  std::uint64_t compaction_generation{};
  std::uint64_t request_epoch_generation{};
  std::optional<RequestOwnerEpoch> request_owner_epoch;
  std::uint64_t compacted_through_transaction_sequence{};
  std::uint64_t compacted_through_owner_event_sequence{};
  std::size_t compacted_structural_completion_count{};
  std::size_t compacted_maintenance_completion_count{};
  std::uint64_t recovered_through_transaction_sequence{};
  std::optional<ProjectionRevisionDTO> checkpoint_projection;
  std::optional<LogicalSceneSnapshotDTO> checkpoint_snapshot;
};

[[nodiscard]] BoundedHistoryState ReadBoundedHistoryState(sqlite3 *database);

SceneProjectionCommitStatusDTO
DecodeProjectionStatus(const std::string_view value);

std::optional<StoredSceneOutcome>
ReadOutcomeForRequest(sqlite3 *database,
                      const std::string_view request_owner_epoch,
                      const RequestId &request_id);

SceneTransactionStoreState
ReadSceneTransactionStoreState(sqlite3 *database,
                               const DurableState &durable_state);

ErrorDTO SceneError(const std::string_view code, const std::string_view message,
                    std::map<std::string, std::string> details = {});

SceneCommitResultDTO
FailedSceneResult(const RequestId &request_id,
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

void InsertSceneOutcome(sqlite3 *database,
                        const std::string_view request_owner_epoch,
                        const RequestId &request_id,
                        const ContentDigest &request_digest,
                        const std::optional<std::uint64_t> transaction_sequence,
                        const bool success,
                        const SceneProjectionCommitStatusDTO projection_status,
                        const std::size_t attempted_projection_count,
                        const std::optional<ErrorDTO> &error);

SceneCommitResultDTO
ResultFromStoredOutcome(sqlite3 *database, const StoredSceneOutcome &outcome,
                        const core::RequestReplayStatusDTO replay_status);

struct RequiredSceneProjection final {
  std::shared_ptr<core::IPlannerSceneProjection> projection;
  core::ProjectionHealthDTO preflight_health;
};

[[nodiscard]] RequiredSceneProjection ResolveRequiredProjection(
    const core::ProjectionNativeRuntimeBindingDTO &binding,
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

bool SettleInterruptedMaintenanceInitializations(sqlite3 *database);

DurableState ReadCompleteDurableState(
    sqlite3 *database,
    const ProjectionBootstrapPrerequisiteQueryDTO &fixed_query);

bool RecoverIncompleteSceneTransaction(
    sqlite3 *database,
    const ProjectionBootstrapPrerequisiteQueryDTO &fixed_query);

void ValidateBaseDatabase(
    sqlite3 *database,
    const ProjectionBootstrapPrerequisiteQueryDTO &fixed_query,
    const ContentDigest &admission_digest);

void ValidateTransactionDatabase(
    sqlite3 *database,
    const ProjectionBootstrapPrerequisiteQueryDTO &fixed_query,
    const ContentDigest &admission_digest);

void ValidateNativeHandoffDatabase(
    sqlite3 *database,
    const ProjectionBootstrapPrerequisiteQueryDTO &fixed_query,
    const ContentDigest &admission_digest);

void ValidateBoundedHistoryDatabase(
    sqlite3 *database,
    const ProjectionBootstrapPrerequisiteQueryDTO &fixed_query,
    const ContentDigest &admission_digest);

void ValidateDatabase(
    sqlite3 *database,
    const ProjectionBootstrapPrerequisiteQueryDTO &fixed_query,
    const ContentDigest &admission_digest);

void ValidateMaintenanceDatabase(
    sqlite3 *database,
    const ProjectionBootstrapPrerequisiteQueryDTO &fixed_query,
    const ContentDigest &admission_digest);

/// Schema and metadata of a v9 store, without decoding its content: v9
/// documents fail current decoding until `ConvertProviderObservationEvidence`.
void ValidateInitialSceneBaselineDatabase(
    sqlite3 *database,
    const ProjectionBootstrapPrerequisiteQueryDTO &fixed_query,
    const ContentDigest &admission_digest);

/// One-time v9 -> v10 conversion inside the caller's write transaction.
void ConvertProviderObservationEvidence(sqlite3 *database);

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

[[nodiscard]] const SceneObjectSnapshotDTO *
FindSceneObject(const LogicalSceneSnapshotDTO &snapshot,
                const core::SceneResourceId &scene_resource_id);

} // namespace manyforge::scene::logical_scene_owner_detail

namespace manyforge::scene {
using namespace logical_scene_owner_detail;
} // namespace manyforge::scene
