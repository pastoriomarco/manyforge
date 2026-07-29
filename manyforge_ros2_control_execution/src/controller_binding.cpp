#include <manyforge_ros2_control_execution/controller_binding.hpp>

#include <algorithm>
#include <sstream>
#include <utility>

namespace manyforge::ros2_control_execution {
namespace {

std::string join(const std::vector<std::string>& values) {
  std::ostringstream stream;
  for (std::size_t index = 0; index < values.size(); ++index) {
    if (index != 0) {
      stream << ',';
    }
    stream << values[index];
  }
  return stream.str();
}

DispatchAdmissionDecision rejected(
    std::string reason_code,
    std::string message,
    std::map<std::string, std::string> details = {}) {
  DispatchAdmissionDecision result;
  result.reason_code = std::move(reason_code);
  result.message = std::move(message);
  result.details = std::move(details);
  return result;
}

DispatchAdmissionDecision evaluate_current(
    const ControllerBindingContract* contract,
    const ControllerBindingSnapshot* binding,
    const ExecutionAdmissionSnapshot* admission,
    bool require_execution_admission,
    std::int64_t now_ns) {
  if (contract != nullptr) {
    if (binding == nullptr || binding->generation == 0) {
      return rejected("controller_binding_unavailable",
                      "No controller binding snapshot is available");
    }
    if (binding->contract_generation_id != contract->generation_id) {
      return rejected(
          "controller_binding_contract_mismatch",
          "Controller binding snapshot belongs to a different contract",
          {{"expected_binding_contract_generation_id", contract->generation_id},
           {"observed_binding_contract_generation_id",
            binding->contract_generation_id}});
    }
    if (binding->fresh_until_ns <= now_ns) {
      return rejected(
          "controller_binding_stale",
          "Controller binding snapshot is stale",
          {{"binding_snapshot_generation",
            std::to_string(binding->generation)},
           {"binding_fresh_until_ns",
            std::to_string(binding->fresh_until_ns)},
           {"dispatch_timestamp_ns", std::to_string(now_ns)}});
    }
    if (!binding->valid) {
      return rejected(
          "controller_binding_invalid",
          "Controller binding snapshot is not dispatchable",
          {{"binding_snapshot_generation",
            std::to_string(binding->generation)},
           {"binding_reason_codes", join(binding->reason_codes)}});
    }
  }

  if (require_execution_admission) {
    if (admission == nullptr || admission->generation == 0) {
      return rejected("execution_admission_unavailable",
                      "No lifecycle execution-admission snapshot is available");
    }
    if (admission->fresh_until_ns <= now_ns) {
      return rejected(
          "execution_admission_stale",
          "Lifecycle execution-admission snapshot is stale",
          {{"admission_snapshot_generation",
            std::to_string(admission->generation)},
           {"admission_fresh_until_ns",
            std::to_string(admission->fresh_until_ns)},
           {"dispatch_timestamp_ns", std::to_string(now_ns)}});
    }
    if (contract != nullptr &&
        admission->binding_contract_generation_id !=
            contract->generation_id) {
      return rejected(
          "execution_admission_binding_mismatch",
          "Lifecycle admission belongs to a different controller binding",
          {{"expected_binding_contract_generation_id", contract->generation_id},
           {"admission_binding_contract_generation_id",
            admission->binding_contract_generation_id}});
    }
    if (!admission->controller_ready) {
      return rejected(
          "execution_controller_not_ready",
          "Lifecycle authority reports that the controller is not ready",
          {{"admission_snapshot_generation",
            std::to_string(admission->generation)},
           {"admission_reason_codes", join(admission->reason_codes)}});
    }
    if (!admission->motion_admitted) {
      return rejected(
          "execution_motion_not_admitted",
          "Lifecycle authority has not admitted motion",
          {{"admission_snapshot_generation",
            std::to_string(admission->generation)},
           {"admission_reason_codes", join(admission->reason_codes)}});
    }
    if (admission->runtime_generation_id.empty() ||
        admission->provider_component_epoch.empty() ||
        admission->transport_owner_component_epoch.empty() ||
        admission->authority_component_epoch.empty() ||
        admission->cell_supervisor_component_epoch.empty()) {
      return rejected(
          "execution_environment_identity_missing",
          "Lifecycle admission is missing a required component epoch",
          {{"admission_snapshot_generation",
            std::to_string(admission->generation)}});
    }
  }

  DispatchAdmissionDecision result;
  result.admitted = true;
  return result;
}

}  // namespace

DispatchAdmissionDecision evaluate_dispatch_admission(
    const ControllerBindingContract* contract,
    const ControllerBindingSnapshot* binding,
    const ExecutionAdmissionSnapshot* admission,
    bool require_execution_admission,
    std::int64_t now_ns,
    const std::string& configured_action_name,
    const std::vector<std::string>& trajectory_joint_names) {
  if (contract != nullptr) {
    if (configured_action_name !=
        contract->follow_joint_trajectory_action) {
      return rejected(
          "controller_action_binding_mismatch",
          "Configured FJT action disagrees with the controller binding",
          {{"expected_action", contract->follow_joint_trajectory_action},
           {"configured_action", configured_action_name}});
    }
    if (trajectory_joint_names != contract->required_joints) {
      return rejected(
          "controller_joint_order_mismatch",
          "Trajectory joint names/order disagree with the controller binding",
          {{"expected_joints", join(contract->required_joints)},
           {"trajectory_joints", join(trajectory_joint_names)}});
    }
  }

  auto result = evaluate_current(
      contract, binding, admission, require_execution_admission, now_ns);
  if (!result.admitted) {
    return result;
  }

  DispatchAuthorityPin pin;
  if (contract != nullptr && binding != nullptr) {
    pin.binding_contract_generation_id = contract->generation_id;
    pin.binding_snapshot_generation = binding->generation;
    pin.binding_activity_generation = binding->activity_generation;
  }
  if (require_execution_admission && admission != nullptr) {
    pin.admission_snapshot_generation = admission->generation;
    pin.lifecycle_generation = admission->lifecycle_generation;
    pin.runtime_generation_id = admission->runtime_generation_id;
    pin.provider_component_epoch = admission->provider_component_epoch;
    pin.transport_owner_component_epoch =
        admission->transport_owner_component_epoch;
    pin.authority_component_epoch = admission->authority_component_epoch;
    pin.cell_supervisor_component_epoch =
        admission->cell_supervisor_component_epoch;
  }
  result.pin = std::move(pin);
  return result;
}

DispatchAdmissionDecision evaluate_active_dispatch_authority(
    const ControllerBindingContract* contract,
    const ControllerBindingSnapshot* binding,
    const ExecutionAdmissionSnapshot* admission,
    bool require_execution_admission,
    std::int64_t now_ns,
    const DispatchAuthorityPin& pin) {
  auto result = evaluate_current(
      contract, binding, admission, require_execution_admission, now_ns);
  if (!result.admitted) {
    return result;
  }
  if (contract != nullptr && binding != nullptr) {
    if (pin.binding_contract_generation_id != contract->generation_id ||
        pin.binding_activity_generation != binding->activity_generation) {
      return rejected(
          "controller_binding_generation_changed",
          "Controller binding changed after trajectory dispatch",
          {{"dispatch_binding_snapshot_generation",
            std::to_string(pin.binding_snapshot_generation)},
           {"dispatch_binding_activity_generation",
            std::to_string(pin.binding_activity_generation)},
           {"current_binding_activity_generation",
            std::to_string(binding->activity_generation)}});
    }
  }
  if (require_execution_admission && admission != nullptr) {
    if (pin.runtime_generation_id != admission->runtime_generation_id ||
        pin.provider_component_epoch !=
            admission->provider_component_epoch ||
        pin.transport_owner_component_epoch !=
            admission->transport_owner_component_epoch ||
        pin.authority_component_epoch !=
            admission->authority_component_epoch ||
        pin.cell_supervisor_component_epoch !=
            admission->cell_supervisor_component_epoch) {
      return rejected(
          "execution_environment_epoch_changed",
          "A pinned execution component was replaced after dispatch",
          {{"dispatch_runtime_generation_id",
            pin.runtime_generation_id},
           {"current_runtime_generation_id",
            admission->runtime_generation_id},
           {"dispatch_provider_component_epoch",
            pin.provider_component_epoch},
           {"current_provider_component_epoch",
            admission->provider_component_epoch},
           {"dispatch_transport_owner_component_epoch",
            pin.transport_owner_component_epoch},
           {"current_transport_owner_component_epoch",
            admission->transport_owner_component_epoch},
           {"dispatch_authority_component_epoch",
            pin.authority_component_epoch},
           {"current_authority_component_epoch",
            admission->authority_component_epoch},
           {"dispatch_cell_supervisor_component_epoch",
            pin.cell_supervisor_component_epoch},
           {"current_cell_supervisor_component_epoch",
            admission->cell_supervisor_component_epoch}});
    }
    if (pin.lifecycle_generation != admission->lifecycle_generation) {
      return rejected(
          "execution_admission_generation_changed",
          "Lifecycle admission changed after trajectory dispatch",
          {{"dispatch_admission_snapshot_generation",
            std::to_string(pin.admission_snapshot_generation)},
           {"current_admission_snapshot_generation",
            std::to_string(admission->generation)},
           {"dispatch_lifecycle_generation",
            std::to_string(pin.lifecycle_generation)},
           {"current_lifecycle_generation",
            std::to_string(admission->lifecycle_generation)}});
    }
  }
  return result;
}

bool operator==(const ExecutionAdmissionSnapshot& lhs,
                const ExecutionAdmissionSnapshot& rhs) {
  return lhs.generation == rhs.generation &&
         lhs.lifecycle_generation == rhs.lifecycle_generation &&
         lhs.robot_id == rhs.robot_id &&
         lhs.binding_contract_generation_id ==
             rhs.binding_contract_generation_id &&
         lhs.runtime_generation_id == rhs.runtime_generation_id &&
         lhs.provider_component_epoch == rhs.provider_component_epoch &&
         lhs.transport_owner_component_epoch ==
             rhs.transport_owner_component_epoch &&
         lhs.authority_component_epoch == rhs.authority_component_epoch &&
         lhs.cell_supervisor_component_epoch ==
             rhs.cell_supervisor_component_epoch &&
         lhs.observed_timestamp_ns == rhs.observed_timestamp_ns &&
         lhs.received_timestamp_ns == rhs.received_timestamp_ns &&
         lhs.fresh_until_ns == rhs.fresh_until_ns &&
         lhs.controller_ready == rhs.controller_ready &&
         lhs.motion_admitted == rhs.motion_admitted &&
         lhs.reason_codes == rhs.reason_codes;
}

}  // namespace manyforge::ros2_control_execution
