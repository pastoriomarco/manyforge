#pragma once

#include <manyforge_core/collision/i_collision_world_snapshot_publisher.hpp>
#include <manyforge_core/types/scene_transaction.hpp>

namespace manyforge::core {

/// Focused native participant in the logical-scene commit protocol.
///
/// N5b establishes transaction acknowledgement and health. Revision-bound
/// leases, rebuild, structural stage/activation, and native query projection
/// are added through this same focused boundary in N5c.
class IPlannerSceneProjection {
 public:
  virtual ~IPlannerSceneProjection() = default;

  [[nodiscard]] virtual ProjectionCommitResultDTO ApplyTransaction(
      const SceneTransactionDTO& transaction) = 0;

  [[nodiscard]] virtual Result<CollisionWorldLeaseInventory> RetainProjection(
      const ProjectionRevisionDTO& projection_revision,
      ProjectionLeaseId projection_lease_id) = 0;

  virtual void ReleaseProjection(
      const ProjectionLeaseId& projection_lease_id) = 0;

  [[nodiscard]] virtual ProjectionHealthDTO ProjectionHealth() const = 0;

  [[nodiscard]] virtual ProjectionCommitResultDTO RebuildProjection(
      const NativeProjectionRebuildDTO& request) = 0;
};

}  // namespace manyforge::core
