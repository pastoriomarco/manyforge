#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

#include <manyforge_core/ports/iactive_bundle_publication_store.hpp>
#include <manyforge_core/ports/imanager_scene_projection.hpp>

namespace manyforge::deployment {
class CanonicalEmptyRetainedBundleAdmission;
class IVerifiedRetainedBundleSource;
} // namespace manyforge::deployment

namespace manyforge::scene {

inline constexpr std::string_view kCanonicalEmptySceneStoreSchemaId =
    "manyforge.internal.logical-scene.i4.v2";
inline constexpr std::int64_t kCanonicalEmptySceneStoreSchemaVersion = 2;
inline constexpr std::int64_t kCanonicalEmptySceneSqliteApplicationId =
    1296454467; // "MFSC"
inline constexpr std::size_t kMaximumSceneStoreIdentifierBytes = 256U;
inline constexpr std::int64_t kMaximumSceneStoreDatabasePageImageBytes =
    64LL * 1024LL * 1024LL;
inline constexpr std::int64_t kMaximumSceneStoreWalBytes =
    16LL * 1024LL * 1024LL;
inline constexpr std::size_t kMaximumNamedApplySceneStages = 256U;

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
  kNamedApplyStaged,
  kNamedApplyActivated,
  kNamedApplyDiscarded,
  kNamedApplyFinalized,
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
  std::int64_t named_apply_stage_count;
  std::int64_t named_apply_activation_count;
  std::int64_t named_apply_discard_count;
  std::int64_t named_apply_finalization_count;
  CanonicalEmptySceneAccessStatus access;
};

/// Production owner for the exact zero-native I4 logical-scene lineage.
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
/// publication cannot begin until the scene owner has admitted and quiesced
/// its corresponding transition. During initial bootstrap no successor can be
/// admitted before this Finalize completes.
///
/// With an independently verified retained-bundle source, the same owner also
/// admits the first zero-native named-Apply stage, atomically closes writes and
/// leases, captures the current immutable scene revision, and durably returns
/// one replayable staged candidate. Nonzero native materialization remains
/// unavailable until its exact adapter boundary is installed. This implements
/// no public scene edit, compatibility wrapper, ROS adapter, robot command, or
/// legacy SceneAuthority fallback.
class CanonicalEmptySceneOwner final
    : public manyforge::core::IManagerSceneProjectionBootstrapPort,
      public manyforge::core::IManagerSceneProjectionHandoffPort {
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
      const manyforge::deployment::IVerifiedRetainedBundleSource
          &retained_bundle_source);
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

  [[nodiscard]] manyforge::core::ProjectionApplyStageResultDTO
  StageCandidateProjection(
      const manyforge::core::ProjectionApplyStageDTO &request) override;
  [[nodiscard]] manyforge::core::ProjectionApplyActivationResultDTO
  ActivateStagedProjection(
      const manyforge::core::ProjectionApplyActivationDTO &request) override;
  [[nodiscard]] manyforge::core::ProjectionApplyDiscardResultDTO
  DiscardStagedProjection(
      const manyforge::core::ProjectionApplyDiscardDTO &request) override;
  [[nodiscard]] manyforge::core::ProjectionApplyFinalizeResultDTO
  FinalizeProjectionTransition(
      const manyforge::core::ProjectionApplyFinalizeDTO &request) override;
  [[nodiscard]] manyforge::core::ProjectionApplyStatusDTO ProjectionApplyStatus(
      const manyforge::core::ProjectionApplyStatusQueryDTO &query)
      const override;

  [[nodiscard]] CanonicalEmptySceneAccessStatus ReadAccessStatus() const;
  [[nodiscard]] CanonicalEmptySceneStoreIntegrityReport VerifyIntegrity() const;

  void Close();

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace manyforge::scene
