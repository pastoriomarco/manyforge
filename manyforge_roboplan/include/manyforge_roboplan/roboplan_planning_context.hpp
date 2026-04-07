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

  std::vector<SceneTraceEvent> get_scene_trace(size_t limit = 0);
  void clear_scene_trace();

 private:
  struct TrackedObjectState {
    core::Pose3DTO pose;
    std::string frame_id;
    bool attached = false;
  };

  Eigen::Matrix4d pose_to_matrix(const core::Pose3DTO& pose);
  core::Pose3DTO matrix_to_pose(const Eigen::Matrix4d& matrix);
  Eigen::Matrix4d frame_to_world_matrix_locked(const std::string& frame_id);
  core::Pose3DTO pose_in_base_from_tracked_locked(const TrackedObjectState& state,
                                                  const std::string& base_frame);
  bool infer_attached_from_frame(const std::string& frame_id) const;
  core::SceneEditResultDTO reparent_object_locked(const std::string& object_id,
                                                  const std::string& target_frame_id);
  void record_scene_event_locked(SceneTraceEvent event);

  std::shared_ptr<roboplan::Scene> scene_;
  std::string group_name_;
  std::shared_ptr<std::mutex> scene_mutex_;
  std::map<std::string, TrackedObjectState> object_states_;
  std::deque<SceneTraceEvent> scene_trace_;
};

}  // namespace manyforge::roboplan_backend
