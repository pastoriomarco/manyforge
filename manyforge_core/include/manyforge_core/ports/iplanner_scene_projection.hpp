#pragma once

#include <string>
#include <vector>

#include <manyforge_core/collision/i_collision_world_snapshot_publisher.hpp>
#include <manyforge_core/types/error_code.hpp>
#include <manyforge_core/types/scene_authority.hpp>

namespace manyforge::core {

class IPlannerSceneProjection {
 public:
  virtual ~IPlannerSceneProjection() = default;

  virtual SceneProjectionCommitResult apply_transaction(
      const SceneTransactionDTO& transaction,
      const SceneRevisionDTO& proposed_revision) = 0;
  virtual Result<CollisionWorldLeaseInventory> retain_projection(
      const SceneRevisionDTO& revision, const std::string& retention_id) = 0;
  virtual void release_projection(const std::string& retention_id) = 0;
  virtual SceneProjectionHealthDTO projection_health() const = 0;
  virtual SceneProjectionCommitResult rebuild_projection(
      const LogicalSceneSnapshot& snapshot,
      const SceneRevisionDTO& proposed_revision) = 0;

  virtual std::vector<SceneObjectSnapshotDTO> scene_objects() const = 0;
  virtual std::vector<SceneRobotPlacementDTO> robot_placements() const = 0;
  virtual CollisionPolicySnapshot configured_collision_policy() const = 0;
  virtual Result<Pose3DTO> object_pose(
      const std::string& object_id, const std::string& base_frame,
      const SceneRevisionDTO& expected_revision) const = 0;
};

}  // namespace manyforge::core
