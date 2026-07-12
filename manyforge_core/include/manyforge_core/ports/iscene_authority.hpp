#pragma once

#include <string>
#include <vector>

#include <manyforge_core/collision/i_collision_world_snapshot_publisher.hpp>
#include <manyforge_core/types/scene_authority.hpp>

namespace manyforge::core {

class ISceneAuthority : public ICollisionWorldSnapshotPublisher {
 public:
  ~ISceneAuthority() override = default;

  virtual SceneCommitResultDTO commit(SceneTransactionDTO transaction) = 0;
  virtual Result<SceneRevisionDTO> rebuild_projection() = 0;
  virtual SceneRevisionDTO current_revision() const = 0;
  virtual SceneAuthorityStatusDTO authority_status() const = 0;
  virtual LogicalSceneSnapshotPtr logical_snapshot() const = 0;

  virtual SceneEditResultDTO add_object(const AddObjectRequestDTO& request) = 0;
  virtual SceneEditResultDTO remove_object(
      const RemoveObjectRequestDTO& request) = 0;
  virtual SceneEditResultDTO update_object_pose(
      const UpdateObjectPoseRequestDTO& request) = 0;
  virtual SceneEditResultDTO attach_object(
      const AttachObjectRequestDTO& request) = 0;
  virtual SceneEditResultDTO detach_object(
      const DetachObjectRequestDTO& request) = 0;
  virtual SceneEditResultDTO set_robot_base_pose(
      const SetRobotBasePoseRequestDTO& request) = 0;
  virtual SceneEditResultDTO set_collision_policy(
      const SetCollisionPolicyRequestDTO& request) = 0;

  virtual std::vector<SceneObjectSnapshotDTO> scene_objects() const = 0;
  virtual std::vector<SceneRobotPlacementDTO> robot_placements() const = 0;
  virtual Result<Pose3DTO> object_pose(
      const std::string& object_id, const std::string& base_frame) const = 0;
  virtual ObjectStateDTO object_state(const std::string& object_id) const = 0;
};

}  // namespace manyforge::core
