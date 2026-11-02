#pragma once

#include <manyforge_core/collision/i_collision_world_snapshot_publisher.hpp>
#include <manyforge_core/types/scene_frame_resolution.hpp>
#include <manyforge_core/types/scene_transaction.hpp>

namespace manyforge::core {

/// Narrow native-projection surface used by owner-private scene candidates.
/// It can resolve a frame at one predicted state and build one private native
/// successor, but cannot publish, retain, activate, or commit scene truth.
class ISceneCandidateProjection {
 public:
  virtual ~ISceneCandidateProjection() = default;

  [[nodiscard]] virtual Result<SceneFrameResolutionReceiptDTO> ResolveCandidateSceneFrame(
      const CandidateSceneFrameResolutionQueryDTO& query) const = 0;

  [[nodiscard]] virtual Result<CollisionWorldLeaseInventory> ProjectSceneCandidate(
      const SceneCandidatePreviewDTO& candidate,
      const ProjectionLeaseBindingDTO& predecessor_binding,
      ProjectionLeaseId candidate_lease_id) const = 0;
};

}  // namespace manyforge::core
