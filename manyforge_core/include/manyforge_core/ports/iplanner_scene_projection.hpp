#pragma once

#include <manyforge_core/collision/i_collision_world_snapshot_publisher.hpp>
#include <manyforge_core/types/native_projection_handoff.hpp>
#include <manyforge_core/types/scene_transaction.hpp>
#include <memory>

namespace manyforge::core {

/// Focused native participant in the logical-scene commit protocol.
///
/// N5b establishes transaction acknowledgement and health. Revision-bound
/// leases, rebuild, structural stage/activation, and native query projection
/// are added through this same focused boundary in N5c.
class IPlannerSceneProjection {
public:
  virtual ~IPlannerSceneProjection() = default;

  [[nodiscard]] virtual ProjectionCommitResultDTO
  ApplyTransaction(const SceneTransactionDTO &transaction) = 0;

  [[nodiscard]] virtual Result<CollisionWorldLeaseInventory>
  RetainProjection(const ProjectionRevisionDTO &projection_revision,
                   ProjectionLeaseId projection_lease_id) = 0;

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
