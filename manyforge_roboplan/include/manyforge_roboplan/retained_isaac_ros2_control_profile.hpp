#pragma once

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>

#include <manyforge_core/runtime_participant_binding.hpp>
#include <manyforge_deployment/ros2_control_execution_runtime_contract.hpp>
#include <manyforge_roboplan/controller_robot_description.hpp>
#include <manyforge_roboplan/retained_ros2_control_backend_factory.hpp>

namespace manyforge::roboplan_backend {

/// Exact ROS namespace assigned to one retained Isaac controllable component.
/// Component identities that cannot be represented as one bounded ROS token
/// are rejected instead of being lossy-normalized into a crossed endpoint.
[[nodiscard]] std::string retained_isaac_ros_namespace(
    const core::RuntimeParticipantBinding &binding);

/// The Isaac lane's controller robot_description facts of one closed retained
/// profile (Q69): hardware component name, topics and the profile's fixed
/// initial values. Generation from the model URDF and the binding's arm joints
/// is generate_controller_robot_description().
[[nodiscard]] IsaacControllerLane
retained_isaac_controller_lane(std::string_view isaac_profile);

/// Expand one closed retained Isaac profile into the exact ROS execution
/// configuration used by the installed execution host.
///
/// The retained runtime product owns only the small portable profile selector,
/// cadence, seed, and initial state. Endpoint names, controller and hardware
/// identity, the lane's controller facts, the Core participant's lane inputs,
/// the guard topics and the gripper transport are build-owned profile facts.
/// Runtime participant bindings supply the compiled/source coordinate map and
/// are checked exactly here.
[[nodiscard]] RetainedRos2ControlBackendConfig
make_retained_isaac_ros2_control_backend_config(
    const deployment::ros2_control_execution_runtime_contract::Configuration
        &runtime_configuration,
    const core::RuntimeParticipantBinding &motion_binding,
    const std::optional<core::RuntimeParticipantBinding> &gripper_binding);

/// Expand one closed offline mock profile (catalog `mock_profile`) into the
/// ROS execution configuration of its component, structured as the Isaac
/// profile is. Lane facts: Q65 (Isaac's: lead-in `stop_traj_delta_s` 0.1 s,
/// Stop sample period 0.01 s, `joint_state_max_age_s` 0.25 s, tracking
/// tolerance 0.05 rad with timeout 0), Q79 (hard-stop completion and gripper
/// result timeouts per model family; completion policy `position_reached`
/// through the stock GripperActionController's GripperCommand action), the
/// controller cadence read from the Track 2 template the controller runs, and
/// the per-joint cancel deceleration the generated controllers file carries
/// (the deployment's operator Stop profile, Q48), keyed by model source joint.
/// No Isaac guard. A profile without a gripper accepts no gripper binding.
[[nodiscard]] RetainedRos2ControlBackendConfig
make_retained_mock_ros2_control_backend_config(
    const deployment::ros2_control_execution_runtime_contract::Configuration
        &runtime_configuration,
    const core::RuntimeParticipantBinding &motion_binding,
    const std::optional<core::RuntimeParticipantBinding> &gripper_binding,
    const std::map<std::string, double, std::less<>>
        &max_deceleration_on_cancel_by_source_joint);

} // namespace manyforge::roboplan_backend
