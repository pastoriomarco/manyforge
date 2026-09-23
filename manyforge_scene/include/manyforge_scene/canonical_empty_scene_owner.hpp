#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <manyforge_core/ports/iactive_bundle_publication_store.hpp>
#include <manyforge_core/ports/imanager_scene_projection.hpp>
#include <manyforge_core/ports/iplanner_scene_projection.hpp>
#include <manyforge_core/ports/iscene_authority.hpp>
#include <manyforge_core/ports/iscene_candidate_preview.hpp>
#include <manyforge_core/ports/ikernel_cell_admission_port.hpp>
#include <manyforge_core/types/projection_bootstrap.hpp>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace manyforge::deployment {
class CanonicalEmptyRetainedBundleAdmission;
class IVerifiedRetainedBundleSource;
} // namespace manyforge::deployment

namespace manyforge::scene {

inline constexpr std::string_view kCanonicalEmptySceneStoreSchemaId =
    "manyforge.internal.logical-scene.i4.v2";
inline constexpr std::int64_t kCanonicalEmptySceneStoreSchemaVersion = 2;
inline constexpr std::string_view kLogicalSceneStoreSchemaId =
    "manyforge.internal.logical-scene.ua7s.v9";
inline constexpr std::int64_t kLogicalSceneStoreSchemaVersion = 9;
inline constexpr std::int64_t kCanonicalEmptySceneSqliteApplicationId =
    1296454467; // "MFSC"
inline constexpr std::size_t kMaximumSceneStoreIdentifierBytes = 256U;
inline constexpr std::int64_t kMaximumSceneStoreDatabasePageImageBytes =
    64LL * 1024LL * 1024LL;
inline constexpr std::int64_t kMaximumSceneStoreWalBytes =
    16LL * 1024LL * 1024LL;
inline constexpr std::size_t kMaximumMigratedStructuralCompletions = 256U;
inline constexpr std::size_t kMaximumNativeProjectionBindings = 256U;
inline constexpr std::size_t kMaximumMaintenanceInitializations = 256U;
inline constexpr std::size_t kMaximumLogicalSceneTransactions = 256U;
inline constexpr std::size_t kMaximumLogicalSceneTransactionBytes =
    128U * 1024U;

class CanonicalEmptySceneOwnerError : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

class CanonicalEmptySceneOwnerConflictError final
    : public CanonicalEmptySceneOwnerError {
public:
  using CanonicalEmptySceneOwnerError::CanonicalEmptySceneOwnerError;
};

class CanonicalEmptySceneOwnerIntegrityError final
    : public CanonicalEmptySceneOwnerError {
public:
  using CanonicalEmptySceneOwnerError::CanonicalEmptySceneOwnerError;
};

class CanonicalEmptySceneOwnerLimitError final
    : public CanonicalEmptySceneOwnerError {
public:
  using CanonicalEmptySceneOwnerError::CanonicalEmptySceneOwnerError;
};

class CanonicalEmptySceneOwnerBusyError final
    : public CanonicalEmptySceneOwnerError {
public:
  using CanonicalEmptySceneOwnerError::CanonicalEmptySceneOwnerError;
};

class CanonicalEmptySceneOwnerClosedError final
    : public CanonicalEmptySceneOwnerError {
public:
  using CanonicalEmptySceneOwnerError::CanonicalEmptySceneOwnerError;
};

enum class CanonicalEmptySceneBootstrapPhase {
  kInitialCommandFenced,
  kAccepted,
  kReadyToPublish,
  kFinalized,
  kMaintenanceInitializing,
  kMaintenanceCompleted,
};

[[nodiscard]] std::string_view
ToString(CanonicalEmptySceneBootstrapPhase phase) noexcept;

/// Read-only evidence for the scene-write and projection-lease gates owned by
/// this logical-scene database.
///
/// The gates are not mutable booleans. They are closed until the immutable
/// finalization row is durably committed and open exactly when that row and
/// its complete publication receipt pass integrity verification.
struct CanonicalEmptySceneAccessStatus final {
  CanonicalEmptySceneBootstrapPhase phase;
  bool scene_writes_open;
  bool projection_leases_open;
  std::optional<manyforge::core::ProjectionRevisionDTO> active_projection;

  [[nodiscard]] bool valid() const noexcept;
};

struct CanonicalEmptySceneStoreIntegrityReport final {
  manyforge::core::ProjectionBootstrapPrerequisiteQueryDTO fixed_query;
  std::string schema_id;
  std::int64_t schema_version;
  std::string schema_artifact_digest;
  std::int64_t acceptance_row_count;
  std::int64_t rebuild_row_count;
  std::int64_t finalization_row_count;
  std::int64_t migrated_structural_completion_count;
  std::int64_t transaction_intent_count;
  std::int64_t request_outcome_count;
  std::int64_t projection_attempt_count;
  std::int64_t projection_result_count;
  std::int64_t owner_event_count;
  bool projection_desynchronized;
  CanonicalEmptySceneAccessStatus access;
};

/// Production owner for one exact logical-scene lineage.
///
/// Construction accepts only the deployment-owned sealed canonical-empty
/// admission, derives its fixed cell/bundle/graph prerequisite query, and opens
/// a scene-owned SQLite database after the shared mechanical read-only
/// preflight. A caller-authored query or retained bundle cannot establish a
/// parallel scene lineage. The database is separate from every Component
/// Manager store.
/// Begin atomically rechecks the fixed command-fenced empty state, mints the
/// logical lineage through the qualified Core identity codec, and persists the
/// complete revision-one snapshot before returning. Rebuild records exact
/// healthy zero-native evidence. Finalize independently rereads the current
/// active-bundle publication receipt and durably records it before the access
/// gates can become open.
///
/// The publication-head read and this package's scene-database commit remain
/// deliberately separate durable boundaries. Their ordering relies on the
/// Manager port contract: Manager is the sole publication writer, and a later
/// publication cannot begin until the scene owner has admitted and completed
/// its corresponding maintenance initialization. During initial bootstrap no
/// successor can be admitted before this Finalize completes.
///
/// With an independently verified retained-bundle source and an injected
/// projection resolver, the same owner initializes the candidate-only native
/// set during an admitted maintenance window. It also owns durable
/// compare-and-commit logical-scene transactions, immutable reads, exact
/// replay, projection acknowledgement, retained component/passive-frame
/// resolution, carried-object attachment/detachment, immutable projection
/// leases, and collision-policy projection. It does not retain or resume the
/// removed predecessor/candidate Stage/Activate/Finalize protocol. This owner
/// implements no compatibility wrapper, robot command, authorization layer,
/// or legacy SceneAuthority fallback.
class CanonicalEmptySceneOwner final
    : public manyforge::core::IManagerSceneProjectionBootstrapPort,
      public manyforge::core::IManagerSceneProjectionMaintenancePort,
      public manyforge::core::ISceneAuthority,
      public manyforge::core::ISceneCandidatePreview {
public:
  CanonicalEmptySceneOwner(
      std::filesystem::path database_path,
      const manyforge::deployment::CanonicalEmptyRetainedBundleAdmission
          &admission,
      const manyforge::core::IActiveBundlePublicationReader
          &publication_reader);
  CanonicalEmptySceneOwner(
      std::filesystem::path database_path,
      const manyforge::deployment::CanonicalEmptyRetainedBundleAdmission
          &admission,
      const manyforge::core::IActiveBundlePublicationReader &publication_reader,
      manyforge::core::IKernelCellAdmissionPort &cell_admission);
  CanonicalEmptySceneOwner(
      std::filesystem::path database_path,
      const manyforge::deployment::CanonicalEmptyRetainedBundleAdmission
          &admission,
      const manyforge::core::IActiveBundlePublicationReader &publication_reader,
      const manyforge::deployment::IVerifiedRetainedBundleSource
          &retained_bundle_source);
  CanonicalEmptySceneOwner(
      std::filesystem::path database_path,
      const manyforge::deployment::CanonicalEmptyRetainedBundleAdmission
          &admission,
      const manyforge::core::IActiveBundlePublicationReader &publication_reader,
      const manyforge::deployment::IVerifiedRetainedBundleSource
          &retained_bundle_source,
      const manyforge::core::IPlannerSceneProjectionResolver
          &projection_resolver);
  CanonicalEmptySceneOwner(
      std::filesystem::path database_path,
      const manyforge::deployment::CanonicalEmptyRetainedBundleAdmission
          &admission,
      const manyforge::core::IActiveBundlePublicationReader &publication_reader,
      const manyforge::deployment::IVerifiedRetainedBundleSource
          &retained_bundle_source,
      const manyforge::core::IPlannerSceneProjectionResolver
          &projection_resolver,
      manyforge::core::IKernelCellAdmissionPort &cell_admission);
  ~CanonicalEmptySceneOwner() override;

  CanonicalEmptySceneOwner(CanonicalEmptySceneOwner &&) noexcept;
  CanonicalEmptySceneOwner &operator=(CanonicalEmptySceneOwner &&) noexcept;
  CanonicalEmptySceneOwner(const CanonicalEmptySceneOwner &) = delete;
  CanonicalEmptySceneOwner &
  operator=(const CanonicalEmptySceneOwner &) = delete;

  [[nodiscard]] const std::filesystem::path &database_path() const noexcept;
  [[nodiscard]] const manyforge::core::ProjectionBootstrapPrerequisiteQueryDTO &
  fixed_query() const noexcept;

  [[nodiscard]] manyforge::core::ProjectionBootstrapPrerequisiteStatusDTO
  ObserveProjectionBootstrapPrerequisites(
      const manyforge::core::ProjectionBootstrapPrerequisiteQueryDTO &query)
      const override;

  [[nodiscard]] manyforge::core::ProjectionBootstrapAcceptedDTO
  BeginProjectionBootstrap(
      const manyforge::core::ProjectionBootstrapIntentDTO &intent) override;

  [[nodiscard]] manyforge::core::ProjectionBootstrapRebuildResultDTO
  RebuildProjectionBootstrap(
      const manyforge::core::ProjectionBootstrapRebuildRequestDTO &request)
      override;

  [[nodiscard]] manyforge::core::ProjectionBootstrapStatusDTO
  ProjectionBootstrapStatus(
      const manyforge::core::ProjectionBootstrapStatusQueryDTO &query)
      const override;

  [[nodiscard]] manyforge::core::ProjectionBootstrapFinalizeResultDTO
  FinalizeProjectionBootstrap(
      const manyforge::core::ProjectionBootstrapFinalizeDTO &request) override;

  [[nodiscard]]
  manyforge::core::ProjectionMaintenanceInitializationStatusDTO
  BeginMaintenanceInitialization(
      const manyforge::core::ProjectionMaintenanceInitializationRequestDTO
          &request) override;

  [[nodiscard]] manyforge::core::ProjectionMaintenanceInitializationStatusDTO
  InstallInitialScene(
      const manyforge::core::ProjectionMaintenanceInitializationRequestDTO
          &request) override;
  [[nodiscard]]
  manyforge::core::ProjectionMaintenanceInitializationStatusDTO
  CompleteMaintenanceInitialization(
      const manyforge::core::ProjectionMaintenanceCompletionDTO &completion)
      override;
  [[nodiscard]] std::optional<
      manyforge::core::ProjectionMaintenanceInitializationStatusDTO>
  ReadMaintenanceInitialization(
      const manyforge::core::ProjectionMaintenanceInitializationQueryDTO
          &query) const override;
  [[nodiscard]] manyforge::core::ProjectionAggregateHealthDTO
  ActiveProjectionHealth() const override;

  [[nodiscard]] CanonicalEmptySceneAccessStatus ReadAccessStatus() const;
  /// Reconciles durable authored-scene admission claims after the exact
  /// Kernel owner becomes reachable. Owner unavailability is returned as a
  /// typed fail-closed result and never prevents read-only scene recovery.
  [[nodiscard]] manyforge::core::Result<void>
  ReconcileAuthoredSceneAdmissionClaims();
  [[nodiscard]] manyforge::core::SceneCommitResultDTO
  Commit(const manyforge::core::SceneTransactionRequestDTO &request) override;
  /// Exact result lookup only: never submit/retry a Scene effect. Absence is
  /// unresolved, not proof of no effect (the original request may be in flight).
  [[nodiscard]] std::optional<manyforge::core::SceneCommitResultDTO>
  ReadCommitResult(const manyforge::core::SceneTransactionRequestDTO& request) const;
  [[nodiscard]] manyforge::core::Result<manyforge::core::SceneCandidatePreviewDTO>
  PreviewResolvedSceneCandidate(
      const manyforge::core::ResolvedSceneCandidatePreviewRequestDTO &request)
      const override;
  [[nodiscard]] manyforge::core::RequestOwnerEpoch
  CurrentRequestOwnerEpoch() const override;
  [[nodiscard]] manyforge::core::ProjectionRevisionDTO
  CurrentProjectionRevision() const override;
  [[nodiscard]] std::shared_ptr<const manyforge::core::LogicalSceneSnapshotDTO>
  LogicalSnapshot() const override;
  [[nodiscard]] std::vector<manyforge::core::SceneObjectSnapshotDTO>
  GetSceneObjects() const override;
  [[nodiscard]] manyforge::core::SceneObjectStateDTO GetObjectState(
      const manyforge::core::SceneResourceId &scene_resource_id) const override;
  [[nodiscard]] manyforge::core::Result<manyforge::core::Pose3DTO>
  GetObjectPose(
      const manyforge::core::SceneResourceId &scene_resource_id) const override;
  /// Returns the current immutable logical-scene snapshot after bootstrap has
  /// established a real active projection. During maintenance initialization
  /// the snapshot remains immutable and admission stays closed until the
  /// completion record is durable. Before bootstrap Finalize there is no
  /// public logical-scene truth and the result is empty.
  [[nodiscard]] std::optional<manyforge::core::LogicalSceneSnapshotRoot>
  ReadActiveSnapshot() const;
  [[nodiscard]] CanonicalEmptySceneStoreIntegrityReport VerifyIntegrity() const;

  void Close();

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

/// Read-only native-Kernel settlement check for an exact authored-scene
/// claim. It never initializes, migrates, locks, checkpoints, or mutates the
/// scene owner database.
[[nodiscard]] manyforge::core::Result<void>
ValidateAuthoredSceneCellAdmissionSettlementReadOnly(
    const std::filesystem::path &database_path,
    const manyforge::core::CellId &cell_id,
    const manyforge::core::CellAdmissionClaimDTO &claim,
    const manyforge::core::CellAdmissionClaimReleaseDTO &release);

} // namespace manyforge::scene
