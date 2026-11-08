#pragma once

#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <manyforge_core/component_stream_health.hpp>
#include <manyforge_core/runtime_participant_binding.hpp>
#include <manyforge_core/types/active_control_configuration.hpp>
#include <manyforge_deployment/ros2_control_execution_runtime_contract.hpp>
#include <manyforge_roboplan/controller_launch_artifacts.hpp>
#include <manyforge_roboplan/controller_robot_description.hpp>
#include <manyforge_roboplan/retained_ros2_control_backend_factory.hpp>

namespace manyforge::roboplan_backend {


/// Construct a secondary measured query from one verified physical binding
/// and its exact reconciled route. This never changes execution/Stop authority
/// or creates an observation. A genuine source callback supplies the sample.
[[nodiscard]] core::Result<core::MeasuredStateSourceDescriptor>
make_retained_ros2_control_measured_state_descriptor(
    const core::RuntimeParticipantBinding &physical_binding,
    const core::ActiveControlConfigurationDTO &reconciled_configuration,
    const RetainedMeasuredJointStateLane &lane);

/// The component's declared `stream_id` row (budget/severity/recovery
/// defaults); throws when the component does not declare the stream.
[[nodiscard]] const deployment::ros2_control_execution_runtime_contract::
    Configuration::ComponentStream &
retained_component_stream(
    const deployment::ros2_control_execution_runtime_contract::Configuration
        &configuration,
    const core::ControllableComponentId &component, std::string_view stream_id);

/// The component's measured JointState lane on `absolute_topic`, with the
/// budget/severity/recovery of its `measured_joints` stream row.
[[nodiscard]] RetainedMeasuredJointStateLane retained_measured_joint_state_lane(
    const deployment::ros2_control_execution_runtime_contract::Configuration
        &configuration,
    const core::ControllableComponentId &component, std::string absolute_topic);

/// The freshness budget of `motion_binding`'s observed-state channel, as
/// retained_ros2_control_motion_state_config() selects it: the component's
/// `measured_joints` stream budget for a measured authority (Real), 0 for the
/// non-expiring commanded state (Mock, Isaac). The kinematic state projector
/// registers the motion target with this budget, because the channel requires
/// its exact query context there.
[[nodiscard]] core::Duration retained_ros2_control_motion_state_budget_ns(
    const deployment::ros2_control_execution_runtime_contract::Configuration
        &configuration,
    const core::RuntimeParticipantBinding &motion_binding);

/// The machines of `configuration` whose owner no arm member
/// builds (no controller profile names it as owner: its arm is deactivated).
/// The host builds each one's owner as a component of its own, running the
/// machine's process where the machine has one (`deactivated_arm`); in
/// `configuration.machines` order.
[[nodiscard]] std::vector<
    deployment::ros2_control_execution_runtime_contract::Configuration::Machine>
retained_ros2_control_armless_machines(
    const deployment::ros2_control_execution_runtime_contract::Configuration
        &configuration);

/// Exact ROS namespace assigned to one retained controllable component
/// (`/manyforge/<component_id>`, every ros2_control lane). Component
/// identities that cannot be represented as one bounded ROS token are
/// rejected instead of being lossy-normalized into a crossed endpoint.
[[nodiscard]] std::string retained_ros2_control_namespace(
    const core::RuntimeParticipantBinding &binding);

/// `<runtime_namespace>/<relative_name>`.
[[nodiscard]] std::string retained_ros2_control_scoped_name(
    std::string_view runtime_namespace, std::string_view relative_name);

/// The binding's compiled joint names, in binding order; throws for an
/// incomplete coordinate map.
[[nodiscard]] std::vector<std::string> retained_ros2_control_compiled_joints(
    const core::RuntimeParticipantBinding &binding);

/// The binding's one compiled model group; throws naming `description`.
[[nodiscard]] std::string retained_ros2_control_sole_group(
    const core::RuntimeParticipantBinding &binding, const char *description);

/// One value. The component's effective `measured_joints` budget (its
/// row, or the Deployment override the stream registry applies on top of the
/// declared row) is also the participant's in-motion staleness check.
[[nodiscard]] core::Duration retained_ros2_control_measured_joints_budget_ns(
    const RetainedMeasuredJointStateLane &lane,
    const core::ControllableComponentId &component,
    std::span<const core::StreamParameterOverrideDTO> stream_overrides);

/// Shared JTC wiring and Stop policy of every ros2_control lane (Mock,
/// Isaac, Real). Lane builders still own their validation, cancellation
/// bounds and gripper configuration.
[[nodiscard]] RetainedRos2ControlBackendConfig make_ros2_control_jtc_config(
    const std::string &runtime_namespace, std::string_view arm_controller_name,
    std::string_view hardware_component_name,
    const std::vector<std::string> &transport_joints,
    const ControllerLaneRates &rates, double stop_completion_timeout_s,
    core::Duration controller_state_max_age_ns,
    // Whether the lane's controller accepts runtime speed scaling
    // (`jtc_accepts_speed_scaling` of its claimed command interfaces).
    bool supports_scale);

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

/// Populate the existing stock Mock ParallelGripperCommand transport from the exact
/// binding, the tool's lane row (`gripper_result_timeout_s`,
/// `gripper_stop_finish_tolerance`) and its written
/// controllers. No motion participant is constructed.
/// The arm builder and the no-motion owner share these unchanged lane values.
void configure_retained_mock_gripper_action(
    RetainedRos2ControlBackendConfig& config,
    const core::RuntimeParticipantBinding& gripper,
    const ModelLaneRow& row,
    const ControllerLaunchArtifacts& artifacts,
    std::string_view runtime_namespace,
    const std::string& controller_manager);

/// The ROS execution configuration of a Mock component from its model lane
/// row (`hard_stop_completion_timeout_s`, `gripper_result_timeout_s`) and its
/// written launch artifacts (controller names, cadence, reset writer). Lane facts: Isaac's (lead-in `stop_traj_delta_s` 0.1 s,
/// Stop sample period 0.01 s, tracking
/// tolerance 0.05 rad with timeout 0), hard-stop completion and gripper
/// result timeouts per model family; completion policy `position_reached`
/// through the stock parallel gripper action controller's
/// ParallelGripperCommand action, the
/// controller cadence read from the template the controller runs, and
/// the per-joint cancel deceleration the generated controllers file carries
/// (the deployment's operator Stop profile), keyed by model source joint.
/// No Isaac guard. The arm's configuration has no gripper (a tool is its
/// own component).
/// The in-motion staleness check is the effective `measured_joints` budget.
[[nodiscard]] RetainedRos2ControlBackendConfig
make_retained_mock_ros2_control_backend_config(
    const deployment::ros2_control_execution_runtime_contract::Configuration
        &runtime_configuration,
    const ModelLaneRow &row, const ControllerLaunchArtifacts &artifacts,
    const core::RuntimeParticipantBinding &motion_binding,
    const std::map<std::string, double, std::less<>>
        &max_deceleration_on_cancel_by_source_joint,
    std::span<const core::StreamParameterOverrideDTO> stream_overrides = {});

/// The measured-state gates of a model's `lanes.real` row, each a required
/// positive number; throws `std::invalid_argument` naming a missing one (or a
/// missing `arrival_tolerance_rad`).
struct RealLaneRowGates final {
  double joint_state_freshness_s{};
  double start_tolerance_rad{};
  double completion_state_timeout_s{};
  double hard_stop_completion_timeout_s{};
};
[[nodiscard]] RealLaneRowGates real_lane_row_gates(const ModelLaneRow &row);

/// The ROS execution configuration of a Real
/// component from its vendor's `lanes.real` row (`hard_stop_completion_timeout_s`,
/// `joint_state_freshness_s`, `start_tolerance_rad`, `completion_state_timeout_s`,
/// `arrival_tolerance_rad`) and its written launch artifacts. The arm's state
/// authority is measured: the measured JointState lane on `<ns>/joint_states`
/// with the component's `measured_joints` stream row (the same row Mock
/// and Isaac declare; its effective budget also sets the in-motion staleness
/// check) is the addressed arm state.
/// Mock's shared JTC wiring, the profile's controller cadence and the
/// deployment's per-joint cancel deceleration. No scheduled replacement (not qualified on a real robot).
[[nodiscard]] RetainedRos2ControlBackendConfig
make_retained_real_ros2_control_backend_config(
    const deployment::ros2_control_execution_runtime_contract::Configuration
        &runtime_configuration,
    const ModelLaneRow &row, const ControllerLaunchArtifacts &artifacts,
    const core::RuntimeParticipantBinding &motion_binding,
    const std::map<std::string, double, std::less<>>
        &max_deceleration_on_cancel_by_source_joint,
    std::span<const core::StreamParameterOverrideDTO> stream_overrides = {});

} // namespace manyforge::roboplan_backend
