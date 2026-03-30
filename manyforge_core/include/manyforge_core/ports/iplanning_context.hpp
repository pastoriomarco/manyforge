#pragma once

#include <string>

#include <manyforge_core/types/geometry.hpp>
#include <manyforge_core/types/robot_state.hpp>
#include <manyforge_core/types/trajectory_dto.hpp>
#include <manyforge_core/types/validation.hpp>
#include <manyforge_core/types/scene_edit.hpp>

namespace manyforge::core {

/// Planning context interface (spec 220 §3)
/// 
/// Responsibilities:
/// - Provide backend-owned scene/state queries required by the kernel
/// - Current robot state snapshot for execute-time gates
/// - Forward kinematics for link poses
/// - Full trajectory validation against the current backend-owned scene
/// - Collision checks suitable for real-time monitoring
/// - Scene/object edits required by the selected program profile
///
/// Threading:
/// - CollisionCheck() MUST have bounded latency appropriate for monitoring cadence
/// - ValidateTrajectory() MAY take longer but MUST be cancelable/interruptible
/// - No kernel stop/pause call path may block on IPlanningContext locks
class IPlanningContext {
 public:
    virtual ~IPlanningContext() = default;

    /// Get the current robot state snapshot (spec 220 §3)
    /// 
    /// @return RobotStateDTO with current joint positions, velocities, etc.
    virtual RobotStateDTO get_current_state() = 0;

    /// Validate a trajectory against the current scene (spec 220 §3)
    /// 
    /// Pre-execution validation is a mandatory gate that runs before a trajectory
    /// is sent to the execution controller. It may be heavier (checks along the trajectory).
    /// 
    /// @param trajectory The trajectory to validate
    /// @param options Validation options including TCP speed limit and frames
    /// @return ValidationResultDTO indicating validity and any issues
    virtual ValidationResultDTO validate_trajectory(const TrajectoryDTO& trajectory, 
                                                    const ValidationOptionsDTO& options) = 0;

    /// Check for collisions at the current state (spec 220 §3)
    /// 
    /// Real-time collision monitoring runs during execution at a policy-defined cadence
    /// and MUST be suitable for high-rate use.
    /// 
    /// @param request The collision check request
    /// @return CollisionCheckResultDTO indicating collision status
    virtual CollisionCheckResultDTO collision_check(const CollisionCheckRequestDTO& request) = 0;

    /// Compute forward kinematics for a link (spec 220 §3)
    /// 
    /// @param request The FK request with link name and robot state
    /// @return Pose3DTO with the computed pose
    virtual Pose3DTO forward_kinematics(const FkRequestDTO& request) = 0;

    /// Get joint limits for a robot/group (spec 220 §3)
    /// 
    /// @param robot_id The robot identifier
    /// @param group_id The planning group identifier
    /// @return JointLimitsDTO with limits for all joints
    virtual JointLimitsDTO get_joint_limits(const std::string& robot_id, 
                                             const std::string& group_id) = 0;

    /// Add an object to the scene (spec 220 §3)
    /// 
    /// @param request The add object request
    /// @return SceneEditResultDTO indicating success/failure
    virtual SceneEditResultDTO add_object(const AddObjectRequestDTO& request) = 0;

    /// Remove an object from the scene (spec 220 §3)
    /// 
    /// @param request The remove object request
    /// @return SceneEditResultDTO indicating success/failure
    virtual SceneEditResultDTO remove_object(const RemoveObjectRequestDTO& request) = 0;

    /// Update an object's pose in the scene (spec 220 §3)
    /// 
    /// @param request The update object pose request
    /// @return SceneEditResultDTO indicating success/failure
    virtual SceneEditResultDTO update_object_pose(const UpdateObjectPoseRequestDTO& request) = 0;
};

}  // namespace manyforge::core
