#pragma once

#include <manyforge_core/collision/i_collision_world_snapshot_publisher.hpp>
#include <manyforge_core/ports/iscene_candidate_projection.hpp>
#include <manyforge_core/types/native_projection_handoff.hpp>
#include <manyforge_core/types/scene_transaction.hpp>
#include <manyforge_core/types/scene_frame_resolution.hpp>
#include <memory>
#include <utility>

namespace manyforge::core {

/// Focused native participant in the logical-scene commit protocol.
///
/// N5b establishes transaction acknowledgement and health. Revision-bound
/// leases, rebuild, structural stage/activation, and native query projection
/// are added through this same focused boundary in N5c.
class IPlannerSceneProjection : public ISceneCandidateProjection {
public:
  virtual ~IPlannerSceneProjection() = default;

  [[nodiscard]] virtual ProjectionCommitResultDTO
  ApplyTransaction(const SceneTransactionDTO &transaction) = 0;

  [[nodiscard]] virtual Result<CollisionWorldLeaseInventory>
  RetainProjection(const ProjectionRevisionDTO &projection_revision,
                   ProjectionLeaseId projection_lease_id) = 0;

  /// Retain the exact native projection for an owner-side resolution and
  /// return only its binding receipt. Local implementations reuse the full
  /// inventory operation; process proxies keep the inventory native and do
  /// not serialize collision-world internals across the Manager channel.
  [[nodiscard]] virtual Result<ProjectionLeaseBindingDTO>
  RetainProjectionBinding(const ProjectionRevisionDTO &projection_revision,
                          ProjectionLeaseId projection_lease_id) {
    auto retained = RetainProjection(projection_revision,
                                     std::move(projection_lease_id));
    if (!retained.ok()) {
      return Result<ProjectionLeaseBindingDTO>::Err(retained.error());
    }
    return Result<ProjectionLeaseBindingDTO>::Ok(
        retained.value().projection_binding);
  }

  virtual void
  ReleaseProjection(const ProjectionLeaseId &projection_lease_id) = 0;

  [[nodiscard]] virtual ProjectionHealthDTO ProjectionHealth() const = 0;

  [[nodiscard]] virtual ProjectionCommitResultDTO
  RebuildProjection(const NativeProjectionRebuildDTO &request) = 0;

  [[nodiscard]] virtual NativeProjectionBootstrapRebuildResultDTO
  RebuildBootstrapProjection(
      const NativeProjectionBootstrapRebuildDTO &request) = 0;

  [[nodiscard]] virtual NativeProjectionBootstrapStatusDTO
  BootstrapProjectionStatus(
      const NativeProjectionBootstrapStatusQueryDTO &query) const = 0;

  [[nodiscard]] virtual NativeProjectionStageResultDTO
  StageProjection(const NativeProjectionStageDTO &request) = 0;

  [[nodiscard]] virtual NativeProjectionActivationResultDTO
  ActivateStagedProjection(const NativeProjectionActivationDTO &request) = 0;

  [[nodiscard]] virtual NativeProjectionRetireResultDTO
  RetireProjection(const NativeProjectionRetireDTO &request) = 0;

  [[nodiscard]] virtual NativeProjectionRetireStatusDTO ProjectionRetireStatus(
      const NativeProjectionRetireStatusQueryDTO &query) const = 0;

  [[nodiscard]] virtual NativeProjectionDiscardResultDTO
  DiscardStagedProjection(const NativeProjectionDiscardDTO &request) = 0;

  [[nodiscard]] virtual NativeProjectionStatusDTO
  ProjectionStageStatus(const NativeProjectionStatusQueryDTO &query) const = 0;

  [[nodiscard]] virtual Result<SceneFrameResolutionReceiptDTO>
  ResolveSceneFrame(const SceneFrameResolutionQueryDTO &) const {
    return Result<SceneFrameResolutionReceiptDTO>::Err(ErrorDTO{
        "missing_reference", "native scene-frame resolution is unavailable",
        {{"reason", "scene_frame_resolver_not_installed"}}});
  }

  [[nodiscard]] virtual Result<SceneFrameResolutionReceiptDTO>
  ResolveCandidateSceneFrame(
      const CandidateSceneFrameResolutionQueryDTO &) const override {
    return Result<SceneFrameResolutionReceiptDTO>::Err(ErrorDTO{
        "missing_reference",
        "native candidate scene-frame resolution is unavailable",
        {{"reason", "candidate_scene_frame_resolver_not_installed"}}});
  }

  /// Project one owner-issued non-durable scene successor into a private
  /// collision inventory. The predecessor lease remains exact; this call does
  /// not publish, retain, or mutate native scene truth.
  [[nodiscard]] virtual Result<CollisionWorldLeaseInventory>
  ProjectSceneCandidate(
      const SceneCandidatePreviewDTO &,
      const ProjectionLeaseBindingDTO &,
      ProjectionLeaseId) const override {
    return Result<CollisionWorldLeaseInventory>::Err(ErrorDTO{
        "invalid_request", "native scene-candidate projection is unavailable",
        {{"reason", "scene_candidate_projector_not_installed"}}});
  }

  [[nodiscard]] virtual Result<CollisionPolicySnapshot>
  ProjectEffectiveCollisionPolicy(
      const LogicalSceneSnapshotDTO &,
      std::uint64_t effective_policy_revision_id) const {
    static_cast<void>(effective_policy_revision_id);
    return Result<CollisionPolicySnapshot>::Err(ErrorDTO{
        "invalid_request", "native collision-policy projection is unavailable",
        {{"reason", "collision_policy_resolver_not_installed"}}});
  }
};

/// Resolves one exact native projection client selected by retained/durable
/// identity.
///
/// This port deliberately has no enumeration, discovery, placement, or
/// fallback operation. A same-process object, local IPC proxy, or future
/// remote-agent proxy is an adapter detail behind the same result. The caller
/// must still verify the client's reported binding before any native effect.
class IPlannerSceneProjectionResolver {
public:
  virtual ~IPlannerSceneProjectionResolver() = default;

  [[nodiscard]] virtual Result<std::shared_ptr<IPlannerSceneProjection>>
  ResolveProjection(
      const ProjectionNativeRuntimeBindingDTO &native_runtime_binding) const =
      0;
};

} // namespace manyforge::core
