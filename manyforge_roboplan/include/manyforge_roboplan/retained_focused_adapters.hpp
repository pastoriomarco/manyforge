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
#include <manyforge_roboplan/retained_kinematic_state_projector.hpp>
#include <manyforge_roboplan/writer_preferring_shared_gate.hpp>

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

/// One coherent set of computations derived from one whole-cell planning
/// model: the mutable RoboPlan scene, the state projector that writes owner
/// observations into it, and the motion target's kinematics, non-collision
/// validator and collision-state projector bound to that same scene. A
/// generation is immutable once built; refreshing a retained controller
/// builds a complete candidate generation before installation.
struct RetainedKinematicModelGeneration final {
  std::shared_ptr<roboplan::Scene> scene;
  std::shared_ptr<std::mutex> scene_mutex;
  std::shared_ptr<RetainedKinematicStateProjector> state_projector;
  std::shared_ptr<const RetainedRoboPlanKinematicsAdapter> kinematics;
  std::shared_ptr<const RoboPlanTrajectoryValidator> trajectory_validator;
  std::shared_ptr<const RoboPlanCollisionTrajectoryProjector>
      collision_trajectory_projector;
};

/// Build every model-dependent computation of one exact motion target from
/// one scene. Throws if any consumer cannot bind the target in that scene.
[[nodiscard]] std::shared_ptr<const RetainedKinematicModelGeneration>
make_retained_motion_model_generation(
    const core::RuntimeParticipantBinding& motion_binding,
    std::shared_ptr<roboplan::Scene> scene,
    std::shared_ptr<std::mutex> scene_mutex,
    std::shared_ptr<RetainedKinematicStateProjector> state_projector);

/// Stable Kernel- and host-facing owner of the current model generation.
/// The Kernel and direct host queries borrow this object for the runtime's
/// lifetime; every call delegates to exactly one generation. Replacement
/// waits for in-flight calls on the predecessor generation to finish, so no
/// caller ever observes two generations within one call. The gate is
/// writer-preferring: once a replacement waits, new calls wait behind it, so
/// a continuous stream of host queries cannot starve the fenced Apply.
class RetainedKinematicModelProxy final : public core::IRobotKinematics,
                                          public core::ITrajectoryValidator,
                                          public core::ICollisionTrajectoryProjector {
 public:
  explicit RetainedKinematicModelProxy(
      std::shared_ptr<const RetainedKinematicModelGeneration> generation);

  RetainedKinematicModelProxy(const RetainedKinematicModelProxy&) = delete;
  RetainedKinematicModelProxy& operator=(const RetainedKinematicModelProxy&) =
      delete;

  /// Drain in-flight delegated calls, then select the candidate generation.
  void ReplaceGeneration(
      std::shared_ptr<const RetainedKinematicModelGeneration> candidate);

  [[nodiscard]] std::shared_ptr<const RetainedKinematicModelGeneration>
  current_generation() const;

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
  core::ValidationResultDTO validate_non_collision(
      const core::TrajectoryDTO& trajectory,
      const core::ValidationOptionsDTO& options,
      const core::CollisionCancellationToken& cancel) const override;
  core::Result<core::TrajectoryDTO> expand_for_collision(
      const core::TrajectoryDTO& trajectory,
      const core::RobotStateDTO& observed_start_state) const override;

 private:
  mutable WriterPreferringSharedGate mutex_;
  std::shared_ptr<const RetainedKinematicModelGeneration> generation_;
};

}  // namespace manyforge::roboplan_backend
