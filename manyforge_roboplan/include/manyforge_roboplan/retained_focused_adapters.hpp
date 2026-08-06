#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <roboplan/core/scene.hpp>

#include <manyforge_core/ports/icollision_trajectory_projector.hpp>
#include <manyforge_core/ports/irobot_kinematics.hpp>
#include <manyforge_core/ports/itrajectory_validator.hpp>
#include <manyforge_core/runtime_participant_binding.hpp>

namespace manyforge::roboplan_backend {

/// Exact all-angular kinematics projection for one sealed motion target.
class RetainedRoboPlanKinematicsAdapter final
    : public core::IRobotKinematics {
 public:
  RetainedRoboPlanKinematicsAdapter(
      core::RuntimeParticipantBinding binding,
      std::shared_ptr<roboplan::Scene> scene,
      std::shared_ptr<std::mutex> scene_mutex);

  core::Result<core::Pose3DTO> forward_kinematics(
      const core::FkRequestDTO& request) const override;
  core::Result<std::map<std::string, core::Pose3DTO>>
  forward_kinematics_batch(
      const core::RobotStateDTO& robot_state,
      const std::vector<std::string>& target_frames,
      const std::string& base_frame) const override;
  core::Result<core::JointLimitsDTO> joint_limits(
      const std::string& component_id,
      const std::string& command_target_id) const override;
  core::Result<std::vector<std::string>> group_joint_names(
      const std::string& command_target_id) const override;

 private:
  core::RuntimeParticipantBinding binding_;
  std::shared_ptr<roboplan::Scene> scene_;
  std::shared_ptr<std::mutex> scene_mutex_;
  std::string compiled_group_name_;
  std::vector<std::string> compiled_joint_names_;
  std::vector<std::string> permitted_frames_;
};

/// Full historical non-collision trajectory validation, bound once to the
/// exact retained all-angular model group.
class RoboPlanTrajectoryValidator final : public core::ITrajectoryValidator {
 public:
  [[nodiscard]] static std::unique_ptr<RoboPlanTrajectoryValidator> create(
      const core::RuntimeParticipantBinding& binding,
      std::shared_ptr<roboplan::Scene> scene,
      std::shared_ptr<std::mutex> scene_mutex);

  core::ValidationResultDTO validate_non_collision(
      const core::TrajectoryDTO& trajectory,
      const core::ValidationOptionsDTO& options,
      const core::CollisionCancellationToken& cancel) const override;

 private:
  RoboPlanTrajectoryValidator(std::shared_ptr<roboplan::Scene> scene,
                              std::string group_name,
                              std::shared_ptr<std::mutex> scene_mutex);

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

/// Expands one exact target trajectory to the complete retained model state
/// consumed by ManyForge collision engines. Selection is sealed at creation.
class RoboPlanCollisionTrajectoryProjector final
    : public core::ICollisionTrajectoryProjector {
 public:
  [[nodiscard]] static std::unique_ptr<
      RoboPlanCollisionTrajectoryProjector>
  create(const core::RuntimeParticipantBinding& binding,
         std::shared_ptr<roboplan::Scene> scene,
         std::shared_ptr<std::mutex> scene_mutex);

  core::Result<core::TrajectoryDTO> expand_for_collision(
      const core::TrajectoryDTO& trajectory,
      const core::RobotStateDTO& observed_start_state) const override;

 private:
  RoboPlanCollisionTrajectoryProjector(
      std::shared_ptr<roboplan::Scene> scene, std::string group_name,
      std::shared_ptr<std::mutex> scene_mutex)
      : scene_(std::move(scene)),
        group_name_(std::move(group_name)),
        scene_mutex_(std::move(scene_mutex)) {}

  std::shared_ptr<roboplan::Scene> scene_;
  std::string group_name_;
  std::shared_ptr<std::mutex> scene_mutex_;
};

}  // namespace manyforge::roboplan_backend
