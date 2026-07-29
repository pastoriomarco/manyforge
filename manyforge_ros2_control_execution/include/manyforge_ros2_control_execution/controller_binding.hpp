#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace manyforge::ros2_control_execution {

/// Immutable deployment-authority facts compiled from the generated
/// ros2_control artifacts. The generic execution adapter consumes these exact
/// names; it never derives vendor prefixes, namespaces, or controller names.
struct ControllerBindingContract {
  std::string generation_id;
  std::string controller_manager;
  std::string activity_topic;
  std::string hardware_component_name;
  std::string trajectory_controller_manager_name;
  std::string trajectory_controller_ros_name;
  std::string trajectory_controller_type;
  std::string follow_joint_trajectory_action;
  std::string controller_state_topic;
  std::string joint_state_topic;
  std::vector<std::string> required_joints;
  std::vector<std::string> claimed_command_interfaces;
  std::vector<std::string> required_state_interfaces;
  std::int64_t snapshot_max_age_ns = 0;
};

/// One manager-observed, generation-stamped binding verification result.
///
/// `valid` is deliberately redundant with the individual facts and reason
/// codes: dispatch gets one bounded check while diagnostics retain enough
/// information to explain the refusal.
struct ControllerBindingSnapshot {
  std::uint64_t generation = 0;
  std::uint64_t activity_generation = 0;
  std::string contract_generation_id;
  std::int64_t observed_timestamp_ns = 0;
  std::int64_t received_timestamp_ns = 0;
  std::int64_t fresh_until_ns = 0;
  bool controller_active = false;
  bool hardware_active = false;
  bool controller_type_matches = false;
  bool claimed_interfaces_match = false;
  bool state_interfaces_available = false;
  bool conflicting_claims_absent = false;
  bool valid = false;
  std::vector<std::string> reason_codes;
};

/// Robot-family-neutral lifecycle/readiness input supplied by the selected
/// lifecycle authority. This DTO intentionally contains no xArm, UR, PLC, or
/// ROS message types.
struct ExecutionAdmissionSnapshot {
  std::uint64_t generation = 0;
  std::uint64_t lifecycle_generation = 0;
  std::string robot_id;
  std::string binding_contract_generation_id;
  std::string runtime_generation_id;
  std::string provider_component_epoch;
  std::string transport_owner_component_epoch;
  std::string authority_component_epoch;
  std::string cell_supervisor_component_epoch;
  std::int64_t observed_timestamp_ns = 0;
  std::int64_t received_timestamp_ns = 0;
  std::int64_t fresh_until_ns = 0;
  bool controller_ready = false;
  bool motion_admitted = false;
  std::vector<std::string> reason_codes;
};

/// Generations pinned by the exact snapshot read immediately before FJT
/// dispatch. Runtime monitoring compares against this pin; it never queries
/// controller-manager services.
struct DispatchAuthorityPin {
  std::string binding_contract_generation_id;
  std::string runtime_generation_id;
  std::string provider_component_epoch;
  std::string transport_owner_component_epoch;
  std::string authority_component_epoch;
  std::string cell_supervisor_component_epoch;
  std::string execution_backend_component_epoch;
  std::uint64_t binding_snapshot_generation = 0;
  std::uint64_t binding_activity_generation = 0;
  std::uint64_t admission_snapshot_generation = 0;
  std::uint64_t lifecycle_generation = 0;
};

struct DispatchAdmissionDecision {
  bool admitted = false;
  std::string reason_code;
  std::string message;
  std::map<std::string, std::string> details;
  std::optional<DispatchAuthorityPin> pin;
};

/// Pure dispatch gate used by the ROS backend and deterministic tests.
///
/// A null binding contract preserves existing non-physical deployments. When a
/// contract exists, a fresh valid binding is mandatory. Admission is optional
/// only for deployments that explicitly do not require a lifecycle authority.
DispatchAdmissionDecision evaluate_dispatch_admission(
    const ControllerBindingContract* contract,
    const ControllerBindingSnapshot* binding,
    const ExecutionAdmissionSnapshot* admission,
    bool require_execution_admission,
    std::int64_t now_ns,
    const std::string& configured_action_name,
    const std::vector<std::string>& trajectory_joint_names);

/// Recheck a dispatched generation against the latest immutable publications.
/// This function performs no ROS calls and allocates only when producing a
/// failure diagnostic.
DispatchAdmissionDecision evaluate_active_dispatch_authority(
    const ControllerBindingContract* contract,
    const ControllerBindingSnapshot* binding,
    const ExecutionAdmissionSnapshot* admission,
    bool require_execution_admission,
    std::int64_t now_ns,
    const DispatchAuthorityPin& pin);

bool operator==(const ExecutionAdmissionSnapshot& lhs,
                const ExecutionAdmissionSnapshot& rhs);

}  // namespace manyforge::ros2_control_execution
