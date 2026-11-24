#pragma once

#include <cstddef>
#include <cstdint>
#include <manyforge_core/ports/iactive_bundle_publication_store.hpp>
#include <manyforge_core/ports/imanager_scene_projection.hpp>
#include <manyforge_core/ports/iplanner_scene_projection.hpp>
#include <manyforge_core/ports/iscene_authority.hpp>
#include <manyforge_core/ports/iscene_candidate_preview.hpp>
#include <manyforge_core/ports/ikernel_cell_admission_port.hpp>
#include <manyforge_core/ports/imanager_kernel_apply_fence_port.hpp>
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

inline constexpr std::size_t kMaximumSceneStoreIdentifierBytes = 256U;
inline constexpr std::size_t kMaximumNativeProjectionBindings = 256U;
inline constexpr std::size_t kMaximumMaintenanceInitializations = 256U;
inline constexpr std::size_t kMaximumLogicalSceneTransactions = 256U;
/// The largest Scene a Commit accepts, in bytes of its canonical encoding
/// (EncodeLogicalSceneSnapshot); a larger result is refused `scene_too_large`
/// before anything is recorded. The Manager status stream's frame bound is
/// this plus room for the section's access part and envelope, so every
/// committed Scene can be published.
inline constexpr std::size_t kMaximumLogicalSceneSnapshotBytes = 384U * 1024U;

class LogicalSceneOwnerError : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

class LogicalSceneOwnerConflictError final
    : public LogicalSceneOwnerError {
public:
  using LogicalSceneOwnerError::LogicalSceneOwnerError;
};

class LogicalSceneOwnerIntegrityError final
    : public LogicalSceneOwnerError {
public:
  using LogicalSceneOwnerError::LogicalSceneOwnerError;
};

class LogicalSceneOwnerLimitError final
    : public LogicalSceneOwnerError {
public:
  using LogicalSceneOwnerError::LogicalSceneOwnerError;
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
/// the logical-scene owner.
///
/// The gates are not mutable booleans. They are closed until the finalization
/// is recorded and open exactly when it and its complete publication receipt
/// pass integrity verification.
struct CanonicalEmptySceneAccessStatus final {
  CanonicalEmptySceneBootstrapPhase phase;
  bool scene_writes_open;
  bool projection_leases_open;
  std::optional<manyforge::core::ProjectionRevisionDTO> active_projection;

  [[nodiscard]] bool valid() const noexcept;
};

/// One read of the published logical-scene state (OD-302). `read_sequence`
/// rises by one with each read and the reads are taken under the owner's
/// lock, so a higher number holds a state at least as new: the Manager status
/// board keeps the value of the highest number it was given.
struct LogicalScenePublicationRead final {
  std::uint64_t read_sequence;
  CanonicalEmptySceneAccessStatus access;
  std::optional<manyforge::core::LogicalSceneSnapshotRoot> snapshot;
};

struct CanonicalEmptySceneStoreIntegrityReport final {
  manyforge::core::ProjectionBootstrapPrerequisiteQueryDTO fixed_query;
  std::int64_t acceptance_row_count;
  std::int64_t rebuild_row_count;
  std::int64_t finalization_row_count;
  std::int64_t transaction_intent_count;
  std::int64_t request_outcome_count;
  std::int64_t projection_attempt_count;
  std::int64_t projection_result_count;
  std::int64_t owner_event_count;
  bool projection_desynchronized;
  CanonicalEmptySceneAccessStatus access;
};

/// Private continuation input, captured by the original Manager Apply before
/// effects. Scene independently verifies the current Scene, candidate,
/// bindings and still-held parent. This value and recorded rows are never
/// authority.
using QualifiedSceneConfigurationAdoptionRequest =
    manyforge::core::QualifiedSceneConfigurationAdoptionRequestDTO;

/// Production owner for one exact logical-scene lineage.
///
/// Construction accepts only the deployment-owned sealed canonical-empty
/// admission and derives its fixed cell/bundle/graph prerequisite query. A
/// caller-authored query or retained bundle cannot establish a parallel scene
/// lineage. The owner's rows are held in memory only: every owner starts
/// empty, and nothing of the Scene outlives its process.
/// Begin atomically rechecks the fixed command-fenced empty state, mints the
/// logical lineage through the qualified Core identity codec, and records the
/// complete revision-one snapshot before returning. Rebuild records exact
/// healthy zero-native evidence. Finalize independently rereads the current
/// active-bundle publication receipt and records it before the access gates
/// can become open.
///
/// The publication-head read and this owner's record remain separate
/// boundaries. Their ordering relies on the Manager port contract: Manager is
/// the sole publication writer, and a later publication cannot begin until the
/// scene owner has admitted and completed its corresponding maintenance
/// initialization. During initial bootstrap no successor can be admitted
/// before this Finalize completes.
///
/// With an independently verified retained-bundle source and an injected
/// projection resolver, the same owner initializes the candidate-only native
/// set during an admitted maintenance window. It also owns
/// compare-and-commit logical-scene transactions, immutable reads, exact
/// replay, projection acknowledgement, retained component/passive-frame
/// resolution, carried-object attachment/detachment, immutable projection
/// leases, and collision-policy projection. It does not retain or resume the
/// removed predecessor/candidate Stage/Activate/Finalize protocol. This owner
/// implements no compatibility wrapper, robot command, authorization layer,
/// or legacy SceneAuthority fallback.
class LogicalSceneOwner final
    : public manyforge::core::IManagerSceneProjectionBootstrapPort,
      public manyforge::core::IManagerSceneProjectionMaintenancePort,
      public manyforge::core::ISceneAuthority {
public:
  LogicalSceneOwner(
      const manyforge::deployment::CanonicalEmptyRetainedBundleAdmission
          &admission,
      const manyforge::core::IActiveBundlePublicationReader
          &publication_reader);
  LogicalSceneOwner(
      const manyforge::deployment::CanonicalEmptyRetainedBundleAdmission
          &admission,
      const manyforge::core::IActiveBundlePublicationReader &publication_reader,
      manyforge::core::IKernelCellAdmissionPort &cell_admission);
  LogicalSceneOwner(
      const manyforge::deployment::CanonicalEmptyRetainedBundleAdmission
          &admission,
      const manyforge::core::IActiveBundlePublicationReader &publication_reader,
      const manyforge::deployment::IVerifiedRetainedBundleSource
          &retained_bundle_source);
  LogicalSceneOwner(
      const manyforge::deployment::CanonicalEmptyRetainedBundleAdmission
          &admission,
      const manyforge::core::IActiveBundlePublicationReader &publication_reader,
      const manyforge::deployment::IVerifiedRetainedBundleSource
          &retained_bundle_source,
      const manyforge::core::IPlannerSceneProjectionResolver
          &projection_resolver);
  LogicalSceneOwner(
      const manyforge::deployment::CanonicalEmptyRetainedBundleAdmission
          &admission,
      const manyforge::core::IActiveBundlePublicationReader &publication_reader,
      const manyforge::deployment::IVerifiedRetainedBundleSource
          &retained_bundle_source,
      const manyforge::core::IPlannerSceneProjectionResolver
          &projection_resolver,
      manyforge::core::IKernelCellAdmissionPort &cell_admission);
  ~LogicalSceneOwner() override;

  LogicalSceneOwner(LogicalSceneOwner &&) noexcept;
  LogicalSceneOwner &operator=(LogicalSceneOwner &&) noexcept;
  LogicalSceneOwner(const LogicalSceneOwner &) = delete;
  LogicalSceneOwner &
  operator=(const LogicalSceneOwner &) = delete;

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
  /// Only the original captured Manager operation may call these entries while
  /// retaining its operation lifetime, cell claim and configuration fence.
  /// Remote reads detect uncertainty; they do not freeze child status. A failed response must stop parent progression and
  /// retain exclusions, including when this owner's commit already succeeded.
  /// Stages candidate-only native projections from the exact preserved Scene.
  /// This original-operation effect is consumed once by Manager. Accepted
  /// native work is observed through the separate read entry, never retried.
  /// No logical row/revision/publication or controller change occurs here.
  /// Stage/READ use the original persisted Acquire attempt's held-presence
  /// read before candidate configurations exist. Manager must already join
  /// that attempt and its full acceptance to the original live operation.
  /// Original configuration-only Manager preservation. The producer joins the
  /// full persisted intent/direct or authoring claim and retains its lifetime;
  /// copied request/status values never authorize a replacement continuation.
  [[nodiscard]] std::vector<manyforge::core::ProjectionHealthDTO>
  StageOriginalManagerConfigurationAdoption(
      const QualifiedSceneConfigurationAdoptionRequest &request,
      const manyforge::core::IManagerKernelApplyFencePort &original_parent_fence,
      const manyforge::core::ApplyFenceAcquireAttemptQueryDTO &original_fence_attempt);
  [[nodiscard]] std::vector<manyforge::core::ProjectionHealthDTO>
  ReadOriginalManagerConfigurationAdoptionProjectionHealth(
      const QualifiedSceneConfigurationAdoptionRequest &request,
      const manyforge::core::IManagerKernelApplyFencePort &original_parent_fence,
      const manyforge::core::ApplyFenceAcquireAttemptQueryDTO &original_fence_attempt);
  [[nodiscard]] manyforge::core::ProjectionMaintenanceInitializationStatusDTO
  BeginOriginalManagerConfigurationAdoption(
      const QualifiedSceneConfigurationAdoptionRequest &request,
      const manyforge::core::IManagerKernelApplyFencePort &original_parent_fence);
  [[nodiscard]] manyforge::core::ProjectionMaintenanceInitializationStatusDTO
  CompleteOriginalManagerConfigurationAdoption(
      const QualifiedSceneConfigurationAdoptionRequest &request,
      const manyforge::core::ActiveBundlePublicationReceiptDTO &publication,
      const manyforge::core::IManagerKernelApplyFencePort &original_parent_fence);
  /// Ends the adoption of an Apply that ended failed, so none outlives its
  /// Apply. With `publication` (the current head, naming the row's target)
  /// the adoption is completed; without, it ends without publication. A
  /// completed or absent adoption is left as it is. Makes no Kernel, host or
  /// native call.
  void EndOriginalManagerConfigurationAdoption(
      const manyforge::core::ApplyId &apply_id,
      manyforge::core::ApplyGeneration apply_generation,
      const std::optional<manyforge::core::ActiveBundlePublicationReceiptDTO>
          &publication) override;
  [[nodiscard]] manyforge::core::ProjectionAggregateHealthDTO
  ActiveProjectionHealth() const override;

  [[nodiscard]] CanonicalEmptySceneAccessStatus ReadAccessStatus() const;
  /// Reconciles recorded authored-scene admission claims after the exact
  /// Kernel owner becomes reachable. Owner unavailability is returned as a
  /// typed fail-closed result and never prevents read-only scene recovery.
  [[nodiscard]] manyforge::core::Result<void>
  ReconcileAuthoredSceneAdmissionClaims();
  /// Holds the owner lock only to read and write owner state, never across a
  /// host or Kernel call (OD-376). The candidate is validated and digested
  /// once here; each native projection is asked once for its health and the
  /// candidate's effective policy, then applies the owner's delta without
  /// validating it again. The intent is recorded only if no other change
  /// reached the owner meanwhile (compare-and-commit); otherwise, or while
  /// another Commit is in flight, the request is refused `stale_revision`.
  /// Until its outcome, readers see the predecessor and the Apply-lane
  /// writers wait for it. Precondition: one Commit caller, the Manager's
  /// command lane; a second Commit at the same time is refused, not queued.
  [[nodiscard]] manyforge::core::SceneCommitResultDTO
  Commit(const manyforge::core::SceneTransactionRequestDTO &request) override;
  /// Exact result lookup only: never submit/retry a Scene effect. Absence is
  /// unresolved, not proof of no effect (the original request may be in flight).
  [[nodiscard]] std::optional<manyforge::core::SceneCommitResultDTO>
  ReadCommitResult(const manyforge::core::SceneTransactionRequestDTO& request) const;
  [[nodiscard]] manyforge::core::Result<manyforge::core::SceneCandidatePreviewDTO>
  PreviewResolvedSceneCandidate(
      const manyforge::core::ResolvedSceneCandidatePreviewRequestDTO &request) const;
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
  /// completion is recorded. Before bootstrap Finalize there is no
  /// public logical-scene truth and the result is empty.
  [[nodiscard]] std::optional<manyforge::core::LogicalSceneSnapshotRoot>
  ReadActiveSnapshot() const;
  /// ReadAccessStatus and ReadActiveSnapshot of one state, numbered.
  [[nodiscard]] LogicalScenePublicationRead ReadPublication() const;
  [[nodiscard]] CanonicalEmptySceneStoreIntegrityReport VerifyIntegrity() const;
private:
  template <typename Request>
  [[nodiscard]] std::vector<manyforge::core::ProjectionHealthDTO>
  OriginalManagerAdoptionProjectionHealthImpl(
      const Request &request,
      const manyforge::core::IManagerKernelApplyFencePort &original_parent_fence,
      const manyforge::core::ApplyFenceAcquireAttemptQueryDTO &original_fence_attempt,
      bool stage);
  template <typename Request>
  [[nodiscard]] manyforge::core::ProjectionMaintenanceInitializationStatusDTO
  BeginQualifiedAdoptionImpl(
      const Request &request,
      const manyforge::core::IManagerKernelApplyFencePort *original_parent_fence);
  template <typename Request>
  [[nodiscard]] manyforge::core::ProjectionMaintenanceInitializationStatusDTO
  CompleteMaintenanceInitializationImpl(
      const manyforge::core::ProjectionMaintenanceCompletionDTO &completion,
      const Request *adoption,
      const manyforge::core::IManagerKernelApplyFencePort *original_parent_fence);
  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace manyforge::scene
