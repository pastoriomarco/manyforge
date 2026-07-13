#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <roboplan/core/scene.hpp>

#include <manyforge_core/collision/i_collision_diagnostic_comparator.hpp>
#include <manyforge_core/ports/icollision_trajectory_projector.hpp>
#include <manyforge_core/ports/iplanner_scene_projection.hpp>
#include <manyforge_core/ports/ireference_pose_resolver.hpp>
#include <manyforge_core/ports/irobot_kinematics.hpp>
#include <manyforge_core/ports/iscene_authority.hpp>
#include <manyforge_core/ports/itrajectory_validator.hpp>
#include <manyforge_roboplan/roboplan_projection_backend.hpp>

namespace manyforge::roboplan_backend {

class RoboPlanSceneProjection final
    : public core::IPlannerSceneProjection,
      public core::ICollisionWorldSnapshotPublisher {
 public:
  explicit RoboPlanSceneProjection(
      std::shared_ptr<RoboPlanProjectionBackend> services)
      : services_(std::move(services)) {}

  core::SceneProjectionCommitResult apply_transaction(
      const core::SceneTransactionDTO& transaction,
      const core::SceneRevisionDTO& proposed_revision) override;
  core::Result<core::CollisionWorldLeaseInventory> retain_projection(
      const core::SceneRevisionDTO& revision,
      const std::string& retention_id) override;
  void release_projection(const std::string& retention_id) override;
  core::SceneProjectionHealthDTO projection_health() const override;
  core::SceneProjectionCommitResult rebuild_projection(
      const core::LogicalSceneSnapshot& snapshot,
      const core::SceneRevisionDTO& proposed_revision) override;
  std::vector<core::SceneObjectSnapshotDTO> scene_objects() const override;
  std::vector<core::SceneRobotPlacementDTO> robot_placements() const override;
  core::CollisionPolicySnapshot configured_collision_policy() const override;
  core::Result<core::Pose3DTO> object_pose(
      const std::string& object_id,
      const std::string& base_frame,
      const core::SceneRevisionDTO& expected_revision) const override;

  core::CollisionWorldSnapshotPtr snapshot() const override;
  core::CollisionWorldRevision published_collision_world_revision()
      const override;
  std::optional<core::CollisionPolicySnapshot> collision_policy_snapshot()
      const override;
  core::CollisionWorldPolicySnapshotBundle
  collision_world_policy_snapshot_bundle() const override;
  std::optional<core::CollisionWorldLeaseInventory>
  current_scene_snapshot_lease_inventory() const override;
  core::RuntimeCollisionQueryBundlePtr
  published_runtime_collision_query_bundle() const override;

  /// Acquire the exact immutable RoboPlan scene retained for `binding`.
  /// This adapter-private surface is consumed only by the RoboPlan planner;
  /// portable callers retain and pass opaque SceneRevisionBindingDTO values.
  /// Missing, released, or mismatched bindings return nullptr and must fail
  /// planning closed rather than falling forward to the live scene.
  std::shared_ptr<const roboplan::Scene> acquire_native_projection(
      const core::SceneRevisionBindingDTO& binding) const;
  core::Result<core::Pose3DTO> resolve_reference_pose(
      const core::ReferenceDTO& reference,
      const std::string& base_frame,
      const core::SceneRevisionBindingDTO& binding) const;
  std::vector<RoboPlanProjectionBackend::SceneTraceEvent> scene_trace(
      std::size_t limit = 0) const;
  void clear_scene_trace();
  std::map<std::string, std::uint64_t> scene_update_status_counters() const;

 private:
  void mark_desynchronized(std::string reason);

  std::shared_ptr<RoboPlanProjectionBackend> services_;
  mutable std::mutex commit_mutex_;
  mutable std::mutex health_mutex_;
  core::SceneProjectionHealthDTO health_;
};

class RoboPlanKinematicsAdapter final : public core::IRobotKinematics {
 public:
  explicit RoboPlanKinematicsAdapter(
      std::shared_ptr<roboplan::Scene> scene,
      std::string group_name,
      std::shared_ptr<std::mutex> scene_mutex,
      const core::ISceneAuthority& scene_authority,
      std::string robot_id)
      : scene_(std::move(scene)),
        group_name_(std::move(group_name)),
        scene_mutex_(std::move(scene_mutex)),
        scene_authority_(&scene_authority),
        robot_id_(std::move(robot_id)) {}

  core::Result<core::Pose3DTO> forward_kinematics(
      const core::FkRequestDTO& request) const override;
  core::Result<std::map<std::string, core::Pose3DTO>>
  forward_kinematics_batch(
      const core::RobotStateDTO& robot_state,
      const std::vector<std::string>& target_frames,
      const std::string& base_frame) const override;
  core::Result<core::JointLimitsDTO> joint_limits(
      const std::string& robot_id,
      const std::string& group_id) const override;
  core::Result<std::vector<std::string>> group_joint_names(
      const std::string& group_id) const override;

 private:
  std::shared_ptr<roboplan::Scene> scene_;
  std::string group_name_;
  std::shared_ptr<std::mutex> scene_mutex_;
  const core::ISceneAuthority* scene_authority_;
  std::string robot_id_;
};

class RoboPlanReferencePoseResolver final
    : public core::IReferencePoseResolver {
 public:
  explicit RoboPlanReferencePoseResolver(
      std::shared_ptr<RoboPlanSceneProjection> projection)
      : projection_(std::move(projection)) {}

  core::Result<core::Pose3DTO> resolve_reference_pose(
      const core::ReferenceDTO& reference, const std::string& base_frame,
      const core::SceneRevisionBindingDTO& scene_binding) const override;

 private:
  std::shared_ptr<RoboPlanSceneProjection> projection_;
};

class RoboPlanTrajectoryValidator final : public core::ITrajectoryValidator {
 public:
  explicit RoboPlanTrajectoryValidator(
      std::shared_ptr<roboplan::Scene> scene,
      std::string group_name,
      std::shared_ptr<std::mutex> scene_mutex);

  core::ValidationResultDTO validate_non_collision(
      const core::TrajectoryDTO& trajectory,
      const core::ValidationOptionsDTO& options,
      const core::CollisionCancellationToken& cancel) const override;

 private:
  std::shared_ptr<roboplan::Scene> scene_;
  std::string group_name_;
  std::shared_ptr<std::mutex> scene_mutex_;
  std::vector<std::string> model_joint_names_;
  std::vector<double> position_lower_rad_;
  std::vector<double> position_upper_rad_;
  std::vector<double> velocity_limits_rad_s_;
  std::vector<double> acceleration_limits_rad_s2_;
  std::vector<std::uint8_t> continuous_joints_;
};

class RoboPlanCollisionTrajectoryProjector final
    : public core::ICollisionTrajectoryProjector {
 public:
  explicit RoboPlanCollisionTrajectoryProjector(
      std::shared_ptr<roboplan::Scene> scene,
      std::string group_name,
      std::shared_ptr<std::mutex> scene_mutex)
      : scene_(std::move(scene)),
        group_name_(std::move(group_name)),
        scene_mutex_(std::move(scene_mutex)) {}

  core::Result<core::TrajectoryDTO> expand_for_collision(
      const core::TrajectoryDTO& trajectory,
      const core::RobotStateDTO& observed_start_state) const override;

 private:
  std::shared_ptr<roboplan::Scene> scene_;
  std::string group_name_;
  std::shared_ptr<std::mutex> scene_mutex_;
};

class RoboPlanCollisionDiagnosticComparator final
    : public core::ICollisionDiagnosticComparator {
 public:
  using MismatchRecordEmitter =
      std::function<void(core::CollisionEngineMismatchRecord)>;

  explicit RoboPlanCollisionDiagnosticComparator(
      std::shared_ptr<RoboPlanProjectionBackend> services)
      : services_(std::move(services)) {}

  core::CollisionResult compare(
      const core::CollisionQuery& query) const override;

  /// Configure optional shadow comparison evidence. These controls affect
  /// diagnostic output only; the comparator cannot participate in dispatch or
  /// runtime collision authority.
  void set_compare_engine(std::shared_ptr<core::ICollisionQueryEngine> engine);
  void set_mismatch_record_emitter(MismatchRecordEmitter emitter);

 private:
  std::shared_ptr<RoboPlanProjectionBackend> services_;
};

}  // namespace manyforge::roboplan_backend
