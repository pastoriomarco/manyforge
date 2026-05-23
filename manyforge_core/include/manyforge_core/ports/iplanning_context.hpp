#pragma once

#include <string>

#include <manyforge_core/types/collision.hpp>
#include <manyforge_core/types/error_code.hpp>
#include <manyforge_core/types/geometry.hpp>
#include <manyforge_core/types/motion_spec.hpp>
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

    /// Get the current pose of a runtime scene object in the requested base frame.
    ///
    /// This is a backend-authoritative object query surface intended for
    /// object-relative runtime flows and live-connected HMI reconciliation.
    ///
    /// @param object_id Stable object identifier in the backend-owned scene
    /// @param base_frame Requested reference frame for the returned pose
    /// @return Pose3DTO with the current object pose
    virtual Pose3DTO get_object_pose(const std::string& object_id,
                                     const std::string& base_frame) = 0;

    /// Get the current backend-owned existence/attachment state of an object.
    ///
    /// @param object_id Stable object identifier in the backend-owned scene
    /// @return ObjectStateDTO with existence, attachment, and frame identity
    virtual ObjectStateDTO get_object_state(const std::string& object_id) = 0;

    /// Resolve a reference to a backend-authoritative pose in the requested base frame.
    ///
    /// This is the execute-time reference-resolution contract for relative/object-based
    /// motion requests. It MUST resolve against the backend-owned current state/scene.
    ///
    /// @param reference The frame/object/link reference to resolve
    /// @param base_frame Requested reference frame for the returned pose
    /// @return Result<Pose3DTO> with structured missing/ambiguous reference errors
    virtual Result<Pose3DTO> resolve_reference_pose(const ReferenceDTO& reference,
                                                    const std::string& base_frame) = 0;

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

    /// Attach/reparent an object into a new frame while preserving its current world pose.
    ///
    /// @param request The attach request with target frame
    /// @return SceneEditResultDTO indicating success/failure
    virtual SceneEditResultDTO attach_object(const AttachObjectRequestDTO& request) = 0;

    /// Detach/reparent an object into a target frame (typically "universe")
    /// while preserving its current world pose.
    ///
    /// @param request The detach request with target frame
    /// @return SceneEditResultDTO indicating success/failure
    virtual SceneEditResultDTO detach_object(const DetachObjectRequestDTO& request) = 0;

    /// Current authoritative collision-world revision (spec/210 §3.8.1,
    /// COLLISION_CHECKING_OWNERSHIP_PLAN Chunk 2).
    ///
    /// Returns the revision a subsequent collision query / validation /
    /// dispatch would be measured against. The kernel uses this to tag
    /// cache entries at store time and to gate cache hits at lookup time
    /// (a cached trajectory validated under revision N MUST NOT be served
    /// at active revision N+1).
    ///
    /// Threading: implementations MUST be safe to call concurrently with
    /// validate_trajectory / collision_check. Implementations that do not
    /// yet own a revision publisher MAY return the default-constructed
    /// CollisionWorldRevision (`revision_id == 0`, empty `content_hash`);
    /// consumers MUST treat that as "no authoritative revision available
    /// — do not cache".
    virtual CollisionWorldRevision current_collision_world_revision() {
        return CollisionWorldRevision{};
    }

    /// Normalized collision check (spec/210 §3.8.1, COLLISION_CHECKING_OWNERSHIP_PLAN
    /// Chunk 1+). Returns a CollisionResult carrying normalized_pairs, the
    /// active world revision, engine status, and (when a policy snapshot is
    /// installed by the adapter) classified pairs + a CollisionDecision.
    ///
    /// Chunk 5 of the collision-ownership plan elevates this to the port so
    /// the CollisionValidationCoordinator can call it across waypoints
    /// without depending on a specific adapter type. Implementations that
    /// have not yet migrated MUST provide the degraded default that wraps
    /// the legacy collision_check() result; consumers must treat the absence
    /// of normalized_pairs / classified_pairs as kDiagnosticIncomplete.
    virtual CollisionResult collision_check_normalized(
        const CollisionQuery& query) {
        CollisionCheckRequestDTO legacy_req;
        legacy_req.robot_state = query.robot_state;
        auto legacy = collision_check(legacy_req);
        CollisionResult result;
        result.query_id = query.query_id;
        result.surface = query.surface;
        result.policy_revision = query.policy_revision;
        result.state_time_ns = query.state_time_ns;
        result.sampling_mode = query.sampling_mode;
        result.in_collision = legacy.in_collision;
        result.contact_body_a = legacy.contact_body_a;
        result.contact_body_b = legacy.contact_body_b;
        result.engine_status = CollisionEngineStatus::kDiagnosticIncomplete;
        result.decision = legacy.in_collision
                              ? CollisionDecisionResult::kInconclusiveFail
                              : CollisionDecisionResult::kPass;
        result.engine_id = "iplanning_context_legacy_fallback";
        result.engine_version = "0.x";
        return result;
    }
};

}  // namespace manyforge::core
