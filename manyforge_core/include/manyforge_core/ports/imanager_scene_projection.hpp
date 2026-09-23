#pragma once

#include <manyforge_core/types/projection_bootstrap.hpp>
#include <manyforge_core/types/projection_maintenance.hpp>

namespace manyforge::core {

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
