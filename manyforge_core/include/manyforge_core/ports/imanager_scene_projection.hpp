#pragma once

#include <manyforge_core/types/native_projection_handoff.hpp>
#include <manyforge_core/types/projection_apply.hpp>
#include <manyforge_core/types/projection_bootstrap.hpp>

namespace manyforge::core {

/// Named-Apply handoff from Component Manager to the logical-scene owner.
///
/// This port does not expose logical-scene mutation or native planner
/// adapters. The scene owner owns quiesce, immutable capture, stage generation,
/// and the complete predecessor/candidate native transition.
class IManagerSceneProjectionHandoffPort {
 public:
  virtual ~IManagerSceneProjectionHandoffPort() = default;

  /// Returns the exact logical-scene composite that a new named Apply must
  /// freeze as its predecessor.  The active bundle publication deliberately
  /// does not advance for ordinary scene edits, so its scene revision is not
  /// an authoritative substitute for this owner-issued snapshot.
  [[nodiscard]] virtual ProjectionRevisionDTO
  CurrentProjectionRevision() const = 0;

  [[nodiscard]] virtual ProjectionApplyStageResultDTO
  StageCandidateProjection(const ProjectionApplyStageDTO& request) = 0;

  [[nodiscard]] virtual ProjectionApplyActivationResultDTO
  ActivateStagedProjection(const ProjectionApplyActivationDTO& request) = 0;

  [[nodiscard]] virtual ProjectionApplyDiscardResultDTO
  DiscardStagedProjection(const ProjectionApplyDiscardDTO& request) = 0;

  [[nodiscard]] virtual ProjectionApplyFinalizeResultDTO
  FinalizeProjectionTransition(const ProjectionApplyFinalizeDTO& request) = 0;

  [[nodiscard]] virtual ProjectionApplyStatusDTO ProjectionApplyStatus(
      const ProjectionApplyStatusQueryDTO& query) const = 0;

  /// Reconciles only the exact Manager-observed process incarnations for the
  /// already-active structural deployment. The scene owner remains the sole
  /// owner of snapshot selection, native rebuild effects, and durable binding
  /// replacement evidence.
  [[nodiscard]] virtual ActiveProjectionRuntimeRecoveryResultDTO
  ReconcileActiveProjectionRuntimes(
      const ActiveProjectionRuntimeSetDTO& runtime_set) = 0;

  /// Observes the exact current native set after applying any completed
  /// process-replacement recovery. Historical Apply transition evidence is not
  /// rewritten when a host epoch changes.
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
