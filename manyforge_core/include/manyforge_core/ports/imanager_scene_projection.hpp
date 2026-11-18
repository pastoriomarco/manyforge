#pragma once

#include <manyforge_core/types/configuration_fence.hpp>
#include <manyforge_core/types/projection_bootstrap.hpp>
#include <manyforge_core/types/projection_maintenance.hpp>
#include <stdexcept>

namespace manyforge::core {

/// Configuration-only original Manager seal. The producer joins these exact
/// values to its persisted full intent and original live operation. Remote
/// readback never creates that lifetime or authorizes replacement continuation.
struct ManagerConfigurationSceneAuthorityDTO final {
  ApplyFenceOwnerDTO owner;
  CellAdmissionClaimDTO held_cell_claim;
  ConfigurationFenceStatusQueryDTO held_fence_query;

  friend bool operator==(const ManagerConfigurationSceneAuthorityDTO&,
                         const ManagerConfigurationSceneAuthorityDTO&) = default;
};

struct QualifiedSceneConfigurationAdoptionRequestDTO final {
  ManagerConfigurationSceneAuthorityDTO authority;
  ActiveBundlePublicationReceiptDTO expected_predecessor_publication;
  ProjectionRevisionDTO expected_active_projection_revision;
  LogicalSceneSnapshotIdentityDTO expected_snapshot_identity;
  RetainedBundleDTO candidate_retained_bundle;
  std::vector<ProjectionExecutionHostBindingDTO> execution_host_bindings;

  friend bool operator==(const QualifiedSceneConfigurationAdoptionRequestDTO&,
                         const QualifiedSceneConfigurationAdoptionRequestDTO&) = default;
};

/// Focused maintenance-initialization boundary from Component Manager to the
/// logical-scene owner. Scene owns quiescence, snapshot selection, native
/// initialization and completion evidence. Reads are observation-only.
class IManagerSceneProjectionMaintenancePort {
 public:
  virtual ~IManagerSceneProjectionMaintenancePort() = default;

  /// Returns the exact logical-scene composite that a new named Apply must
  /// freeze as its predecessor.  The active bundle publication deliberately
  /// does not advance for ordinary scene edits, so its scene revision is not
  /// an authoritative substitute for this owner-issued snapshot.
  [[nodiscard]] virtual ProjectionRevisionDTO
  CurrentProjectionRevision() const = 0;

  [[nodiscard]] virtual ProjectionMaintenanceInitializationStatusDTO
  BeginMaintenanceInitialization(
      const ProjectionMaintenanceInitializationRequestDTO &request) = 0;

  /// Finish the exact captured Program baseline after the candidate's frame
  /// resolver is privately available under the held Apply fence. This is not
  /// an ordinary authored Scene transaction or a public readiness event.
  [[nodiscard]] virtual ProjectionMaintenanceInitializationStatusDTO
  InstallInitialScene(
      const ProjectionMaintenanceInitializationRequestDTO &request) = 0;

  [[nodiscard]] virtual ProjectionMaintenanceInitializationStatusDTO
  CompleteMaintenanceInitialization(
      const ProjectionMaintenanceCompletionDTO &completion) = 0;

  [[nodiscard]] virtual std::optional<
      ProjectionMaintenanceInitializationStatusDTO>
  ReadMaintenanceInitialization(
      const ProjectionMaintenanceInitializationQueryDTO &query) const = 0;

  /// Configuration preservation: snapshot-bound native staging, invoked
  /// once; an Accepted rebuild is observed through the separate read without
  /// repeating effects. The exact original acquire-attempt query is
  /// observation-only held proof before installation; it does not permit
  /// Acquire retry. The original producer retains its actual claim/fence,
  /// candidate owners and operation lifetime through uncertainty and release.
  /// Uncomposed implementations refuse rather than fake readiness.
  [[nodiscard]] virtual std::vector<ProjectionHealthDTO>
  StageOriginalManagerConfigurationAdoption(
      const QualifiedSceneConfigurationAdoptionRequestDTO&,
      const ApplyFenceAcquireAttemptQueryDTO&) {
    throw std::runtime_error("original Manager Scene configuration staging is unavailable");
  }
  [[nodiscard]] virtual std::vector<ProjectionHealthDTO>
  ReadOriginalManagerConfigurationAdoptionProjectionHealth(
      const QualifiedSceneConfigurationAdoptionRequestDTO&,
      const ApplyFenceAcquireAttemptQueryDTO&) {
    throw std::runtime_error("original Manager Scene configuration staging read is unavailable");
  }
  [[nodiscard]] virtual ProjectionMaintenanceInitializationStatusDTO
  BeginOriginalManagerConfigurationAdoption(const QualifiedSceneConfigurationAdoptionRequestDTO&) {
    throw std::runtime_error("original Manager Scene configuration adoption is unavailable");
  }
  [[nodiscard]] virtual ProjectionMaintenanceInitializationStatusDTO
  CompleteOriginalManagerConfigurationAdoption(
      const QualifiedSceneConfigurationAdoptionRequestDTO&,
      const ActiveBundlePublicationReceiptDTO&) {
    throw std::runtime_error("original Manager Scene configuration adoption is unavailable");
  }
  /// Ends the configuration adoption of an Apply that ended failed: completes
  /// it with the Apply's recorded publication, or ends it without one. Local
  /// to the Scene owner; an absent or completed adoption is left as it is.
  virtual void EndOriginalManagerConfigurationAdoption(
      const ApplyId&, ApplyGeneration,
      const std::optional<ActiveBundlePublicationReceiptDTO>&) {
    throw std::runtime_error("original Manager Scene configuration adoption end is unavailable");
  }

  /// Observes the exact current native set. It never creates or replaces a
  /// missing runtime or projection.
  [[nodiscard]] virtual ProjectionAggregateHealthDTO
  ActiveProjectionHealth() const = 0;
};

/// Focused canonical-empty initialization handoff.
///
/// Begin is strictly no-native-effect. Rebuild is the only bootstrap operation
/// that may invoke native rebuilds, and Finalize independently verifies the
/// durable publication receipt before opening initial writes or leases.
class IManagerSceneProjectionBootstrapPort {
 public:
  virtual ~IManagerSceneProjectionBootstrapPort() = default;

  [[nodiscard]] virtual ProjectionBootstrapPrerequisiteStatusDTO
  ObserveProjectionBootstrapPrerequisites(
      const ProjectionBootstrapPrerequisiteQueryDTO& query) const = 0;

  [[nodiscard]] virtual ProjectionBootstrapAcceptedDTO
  BeginProjectionBootstrap(const ProjectionBootstrapIntentDTO& intent) = 0;

  [[nodiscard]] virtual ProjectionBootstrapRebuildResultDTO
  RebuildProjectionBootstrap(
      const ProjectionBootstrapRebuildRequestDTO& request) = 0;

  [[nodiscard]] virtual ProjectionBootstrapStatusDTO ProjectionBootstrapStatus(
      const ProjectionBootstrapStatusQueryDTO& query) const = 0;

  [[nodiscard]] virtual ProjectionBootstrapFinalizeResultDTO
  FinalizeProjectionBootstrap(
      const ProjectionBootstrapFinalizeDTO& request) = 0;
};

}  // namespace manyforge::core
