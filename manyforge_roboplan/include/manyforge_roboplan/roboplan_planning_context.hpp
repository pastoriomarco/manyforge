#pragma once

#include <memory>
#include <mutex>
#include <string>

#include <roboplan/core/scene.hpp>

#include <manyforge_core/ports/iplanning_context.hpp>

namespace manyforge::roboplan_backend {

/// Native C++ planning context wrapping RoboPlan Scene.
/// Provides state queries, FK, collision checks, validation, and scene edits.
class RoboPlanPlanningContext final : public core::IPlanningContext {
 public:
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

  core::JointLimitsDTO get_joint_limits(const std::string& robot_id,
                                        const std::string& group_id) override;

  core::SceneEditResultDTO add_object(const core::AddObjectRequestDTO& request) override;
  core::SceneEditResultDTO remove_object(const core::RemoveObjectRequestDTO& request) override;
  core::SceneEditResultDTO update_object_pose(const core::UpdateObjectPoseRequestDTO& request) override;

 private:
  Eigen::Matrix4d pose_to_matrix(const core::Pose3DTO& pose);

  std::shared_ptr<roboplan::Scene> scene_;
  std::string group_name_;
  std::shared_ptr<std::mutex> scene_mutex_;
};

}  // namespace manyforge::roboplan_backend
