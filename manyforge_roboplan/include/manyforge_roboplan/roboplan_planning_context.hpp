#pragma once

#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <roboplan/core/scene.hpp>

#include <manyforge_core/ports/iplanning_context.hpp>

namespace manyforge::roboplan_backend {

/// Native C++ planning context wrapping RoboPlan Scene.
/// Provides state queries, FK, collision checks, validation, and scene edits.
class RoboPlanPlanningContext final : public core::IPlanningContext {
 public:
  struct SceneObjectSnapshot {
    std::string object_id;
    core::CollisionShapeDTO shape;
    core::Pose3DTO pose_in_frame;
    core::Pose3DTO pose_in_universe;
    std::string frame_id;
    bool attached = false;
  };

  struct SceneTraceEvent {
    uint64_t timestamp_ns = 0;
    std::string event;
    std::string object_id;
    bool success = false;
    std::optional<std::string> frame_id;
    bool attached = false;
    std::optional<core::Pose3DTO> pose_in_frame;
    std::optional<core::Pose3DTO> pose_in_universe;
    std::optional<std::string> error;
  };

  explicit RoboPlanPlanningContext(std::shared_ptr<roboplan::Scene> scene,
                                  std::string group_name = "",
                                  std::shared_ptr<std::mutex> scene_mutex = nullptr);

  core::RobotStateDTO get_current_state() override;

  core::ValidationResultDTO validate_trajectory(
      const core::TrajectoryDTO& trajectory,
      const core::ValidationOptionsDTO& options) override;

  core::CollisionCheckResultDTO collision_check(
      const core::CollisionCheckRequestDTO& request) override;

  core::Pose3DTO forward_kinematics(const core::FkRequestDTO& request) override;
  core::Pose3DTO get_object_pose(const std::string& object_id,
                                 const std::string& base_frame) override;
  core::ObjectStateDTO get_object_state(const std::string& object_id) override;
  core::Result<core::Pose3DTO> resolve_reference_pose(
      const core::ReferenceDTO& reference,
      const std::string& base_frame) override;

  core::JointLimitsDTO get_joint_limits(const std::string& robot_id,
                                        const std::string& group_id) override;

  core::SceneEditResultDTO add_object(const core::AddObjectRequestDTO& request) override;
  core::SceneEditResultDTO remove_object(const core::RemoveObjectRequestDTO& request) override;
  core::SceneEditResultDTO update_object_pose(const core::UpdateObjectPoseRequestDTO& request) override;
  core::SceneEditResultDTO attach_object(const core::AttachObjectRequestDTO& request) override;
  core::SceneEditResultDTO detach_object(const core::DetachObjectRequestDTO& request) override;

  std::vector<std::string> get_scene_object_ids();
  std::vector<std::string> get_joint_names();
  std::vector<SceneObjectSnapshot> get_scene_objects();
  std::vector<SceneTraceEvent> get_scene_trace(size_t limit = 0);
  void clear_scene_trace();

 private:
  struct TrackedObjectState {
    core::Pose3DTO pose;
    core::CollisionShapeDTO shape;
    std::string frame_id;
    bool attached = false;
    std::vector<std::string> touch_links;
  };

  Eigen::Matrix4d pose_to_matrix(const core::Pose3DTO& pose);
  core::Pose3DTO matrix_to_pose(const Eigen::Matrix4d& matrix);
  Eigen::Matrix4d frame_to_world_matrix_locked(const std::string& frame_id);
  core::Pose3DTO pose_in_base_from_tracked_locked(const TrackedObjectState& state,
                                                  const std::string& base_frame);
  bool infer_attached_from_frame(const std::string& frame_id) const;
  tl::expected<void, std::string> set_touch_links_locked(
      const std::string& object_id,
      const std::vector<std::string>& touch_links,
      bool enable);
  tl::expected<void, std::string> clear_touch_links_locked(
      const std::string& object_id,
      TrackedObjectState& tracked);
  core::SceneEditResultDTO reparent_object_locked(const std::string& object_id,
                                                  const std::string& target_frame_id,
                                                  bool emit_event = true);
  void record_scene_event_locked(SceneTraceEvent event);

  // Mutation-discipline helpers (see notes at top of .cpp and
  // manyforge_specs/docs/implementation/roboplan-adapter-workarounds.md):
  //   - raw_add_locked: appends a geometry to pinocchio via Scene::addBoxGeometry
  //       /addSphereGeometry. Always adds at "universe" first (the only frame
  //       where roboplan's add path passes pinocchio's validation, due to the
  //       swapped FrameIndex/JointIndex argument order in roboplan's call -
  //       see the discipline notes at the top of the .cpp). For attached
  //       targets it then calls Scene::updateGeometryPlacement to stamp the
  //       requested parentFrame and placement. NOTE: roboplan's
  //       updateGeometryPlacement does NOT update parentJoint, so attached
  //       geometries don't follow their parent link in pinocchio's oMg
  //       computation. This matches the pre-fix behavior; fixing it requires
  //       touching roboplan and is out of scope for this patch.
  //   - raw_remove_last_locked: removes the LIFO entry via
  //       Scene::removeGeometry. Only safe if `object_id` is the last entry
  //       in add_order_ (no other index shifts).
  //   - reapply_touch_links_locked: re-disables collision pairs after a re-add.
  //   - erase_object_locked / respawn_object_locked: LIFO drop suffix → mutate
  //       target → replay suffix. Both reapply touch_links per object on
  //       re-add and prune object_states_ on partial-replay failure.
  // Direct calls to scene_->removeGeometry / scene_->updateGeometryPlacement
  // are FORBIDDEN outside these helpers; they trigger upstream roboplan bugs.
  // Sanctioned call sites in the .cpp:
  //   - scene_->updateGeometryPlacement: 1 (raw_add_locked, attached reparent)
  //   - scene_->removeGeometry:          2 (raw_remove_last_locked + raw_add_locked
  //                                        cleanup-on-reparent-failure, which
  //                                        targets the just-added LIFO entry)
  tl::expected<void, std::string> raw_add_locked(const std::string& object_id,
                                                 const TrackedObjectState& state);
  tl::expected<void, std::string> raw_remove_last_locked(const std::string& object_id);
  tl::expected<void, std::string> reapply_touch_links_locked(
      const std::string& object_id, const std::vector<std::string>& touch_links);
  tl::expected<void, std::string> erase_object_locked(const std::string& object_id);
  tl::expected<void, std::string> respawn_object_locked(const std::string& object_id,
                                                        const TrackedObjectState& new_state);
  void prune_object_states_locked();

  std::shared_ptr<roboplan::Scene> scene_;
  std::string group_name_;
  std::shared_ptr<std::mutex> scene_mutex_;
  std::map<std::string, TrackedObjectState> object_states_;
  // Insertion order in pinocchio's geometry vector (excluding URDF-loaded geoms).
  // Maintained by raw_add_locked / raw_remove_last_locked / erase_object_locked.
  // LIFO removal of entries here keeps Scene::collision_geometry_map_ self-consistent.
  std::vector<std::string> add_order_;
  std::deque<SceneTraceEvent> scene_trace_;
};

}  // namespace manyforge::roboplan_backend
