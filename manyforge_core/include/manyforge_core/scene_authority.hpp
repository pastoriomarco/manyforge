#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>

#include <manyforge_core/ports/iplanner_scene_projection.hpp>
#include <manyforge_core/ports/iscene_authority.hpp>

namespace manyforge::core {

struct SceneAuthorityConfig {
  std::string world_frame = "universe";
  std::size_t retained_lease_capacity = 8;
};

class SceneAuthority final : public ISceneAuthority {
 public:
  SceneAuthority(IPlannerSceneProjection& projection,
                 ICollisionWorldSnapshotPublisher& projection_publisher,
                 SceneAuthorityConfig config = {});

  SceneCommitResultDTO commit(SceneTransactionDTO transaction) override;
  Result<SceneRevisionDTO> rebuild_projection() override;
  SceneRevisionDTO current_revision() const override;
  SceneAuthorityStatusDTO authority_status() const override;
  LogicalSceneSnapshotPtr logical_snapshot() const override;

  SceneEditResultDTO add_object(const AddObjectRequestDTO& request) override;
  SceneEditResultDTO remove_object(
      const RemoveObjectRequestDTO& request) override;
  SceneEditResultDTO update_object_pose(
      const UpdateObjectPoseRequestDTO& request) override;
  SceneEditResultDTO attach_object(
      const AttachObjectRequestDTO& request) override;
  SceneEditResultDTO detach_object(
      const DetachObjectRequestDTO& request) override;
  SceneEditResultDTO set_robot_base_pose(
      const SetRobotBasePoseRequestDTO& request) override;
  SceneEditResultDTO set_collision_policy(
      const SetCollisionPolicyRequestDTO& request) override;

  std::vector<SceneObjectSnapshotDTO> scene_objects() const override;
  std::vector<SceneRobotPlacementDTO> robot_placements() const override;
  Result<Pose3DTO> object_pose(const std::string& object_id,
                              const std::string& base_frame) const override;
  ObjectStateDTO object_state(const std::string& object_id) const override;

  CollisionWorldSnapshotPtr snapshot() const override;
  CollisionWorldRevision published_collision_world_revision() const override;
  std::optional<CollisionPolicySnapshot> collision_policy_snapshot()
      const override;
  CollisionWorldPolicySnapshotBundle collision_world_policy_snapshot_bundle()
      const override;
  std::optional<CollisionWorldLeaseInventory>
  current_scene_snapshot_lease_inventory() const override;
  std::optional<CollisionWorldLeaseInventory>
  retain_scene_snapshot_lease_inventory(
      std::string retention_id = {}) override;
  void release_scene_snapshot_lease_inventory(
      const std::string& retention_id) override;
  CollisionWorldLeaseRetentionStats scene_snapshot_lease_retention_stats()
      const override;
  RuntimeCollisionQueryBundlePtr published_runtime_collision_query_bundle()
      const override;

 private:
  SceneCommitResultDTO commit_locked(SceneTransactionDTO transaction);
  template <typename Request>
  SceneEditResultDTO commit_typed(Request request, const char* source);
  bool refresh_committed_publication(
      const SceneRevisionDTO& expected_revision,
      CollisionPolicySnapshot configured_collision_policy,
      std::vector<SceneRobotPlacementDTO> robot_placements,
      std::vector<SceneObjectSnapshotDTO> objects,
      std::uint64_t transaction_sequence,
      std::uint64_t admission_epoch,
      std::optional<std::uint64_t> expected_topology_epoch,
      std::optional<std::uint64_t> expected_policy_epoch,
      ErrorDTO& error);
  static SceneUpdateStatus classify_update(const SceneEditResultDTO& edit);
  static SceneEditResultDTO edit_result(const SceneCommitResultDTO& commit);

  IPlannerSceneProjection& projection_;
  ICollisionWorldSnapshotPublisher& projection_publisher_;
  SceneAuthorityConfig config_;
  mutable std::mutex transaction_mutex_;
  std::uint64_t next_transaction_sequence_ = 0;
  std::uint64_t next_transaction_id_ = 0;
  std::atomic<bool> desynchronized_{false};
  struct CommittedPublication {
    LogicalSceneSnapshotPtr logical;
    CollisionWorldPolicySnapshotBundle collision;
    std::optional<CollisionWorldLeaseInventory> inventory;
    RuntimeCollisionQueryBundlePtr runtime;
  };
  using CommittedPublicationPtr =
      std::shared_ptr<const CommittedPublication>;
  std::atomic<CommittedPublicationPtr> publication_;

  mutable std::mutex retention_mutex_;
  std::deque<CollisionWorldLeaseInventory> retained_inventories_;
  std::atomic<std::uint64_t> next_retention_sequence_{1};
};

}  // namespace manyforge::core
