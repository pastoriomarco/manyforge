#pragma once

#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <manyforge_core/runtime_participant_binding.hpp>
#include <manyforge_core/types/active_control_configuration.hpp>
#include <manyforge_deployment/ros2_control_execution_runtime_contract.hpp>
#include <manyforge_roboplan/controller_robot_description.hpp>
#include <manyforge_roboplan/retained_ros2_control_backend_factory.hpp>

namespace manyforge::roboplan_backend {

struct MockControllerProfileFacts;

/// Construct a secondary measured query from one verified physical binding
/// and its exact reconciled route. This never changes execution/Stop authority
/// or creates an observation. A genuine source callback supplies the sample.
[[nodiscard]] core::Result<core::MeasuredStateSourceDescriptor>
make_retained_ros2_control_measured_state_descriptor(
    const core::RuntimeParticipantBinding &physical_binding,
    const core::ActiveControlConfigurationDTO &reconciled_configuration,
    const RetainedMeasuredJointStateLane &lane);

/// Exact ROS namespace assigned to one retained Isaac controllable component.
/// Component identities that cannot be represented as one bounded ROS token
/// are rejected instead of being lossy-normalized into a crossed endpoint.
[[nodiscard]] std::string retained_isaac_ros_namespace(
    const core::RuntimeParticipantBinding &binding);

/// The Isaac lane's controller robot_description facts of one closed retained
/// profile (Q69): hardware component name and topics, plus the selected
/// deployment's backend-commanded initial pose. Generation from the model URDF
/// and the binding's arm joints is generate_controller_robot_description().
[[nodiscard]] IsaacControllerLane
retained_isaac_controller_lane(
    std::string_view isaac_profile,
    std::span<const double> initial_joint_positions_rad);

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

/// Read only the strict authored row for this exact selected component/target.
/// Coordinate kind and transport names never select actuator semantics.
[[nodiscard]] core::GripperActuationKind retained_ros2_control_gripper_actuation_kind(
    const deployment::ros2_control_execution_runtime_contract::Configuration& configuration,
    const core::RuntimeParticipantBinding& binding);

/// Apply the strict selected output choice after physical lane construction.
/// None removes command transport; it never turns missing configuration into a no-op.
void configure_retained_gripper_output_mode(
    RetainedRos2ControlBackendConfig& config,
    const deployment::ros2_control_execution_runtime_contract::Configuration& configuration,
    const core::RuntimeParticipantBinding& binding);

/// Populate the existing stock Mock GripperCommand transport from the exact
/// binding/profile/controller facts. No motion participant is constructed.
/// The arm builder and the no-motion owner share these unchanged lane values.
void configure_retained_mock_gripper_action(
    RetainedRos2ControlBackendConfig& config,
    const core::RuntimeParticipantBinding& gripper,
    const MockControllerProfileFacts& profile,
    std::string_view runtime_namespace,
    const std::string& controller_manager);

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
