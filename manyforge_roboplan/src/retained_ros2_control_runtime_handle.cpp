#include <manyforge_roboplan/retained_ros2_control_runtime_handle.hpp>

#include <algorithm>
#include <limits>
#include <sstream>
#include <utility>

namespace manyforge::roboplan_backend {
namespace {

template <typename Value>
[[nodiscard]] core::Result<Value> reject(std::string reason,
                                         std::string message) {
  return core::Result<Value>::Err(core::ErrorDTO{
      std::string(core::to_string(core::ErrorCode::kInvariantViolation)),
      std::move(message),
      {{"reason", std::move(reason)}}});
}

[[nodiscard]] core::LifecycleFactDTO boolean_fact(std::string id,
                                                  const bool value) {
  return {core::LifecycleFactId(std::move(id)),
          {core::LifecycleFactValueKindDTO::kBoolean, value, std::nullopt,
           std::nullopt}};
}

[[nodiscard]] std::vector<core::LifecycleFactDTO>
lifecycle_facts(const bool controller_ready, const bool fault_present,
                const bool transport_ready) {
  return {
      boolean_fact("external.controller_configuration_ready", controller_ready),
      boolean_fact("external.fault_present", fault_present),
      boolean_fact("external.transport_ready", transport_ready),
  };
}

[[nodiscard]] bool binding_fault(
    const ros2_control_execution::ControllerBindingSnapshot &snapshot) {
  return std::any_of(
      snapshot.reason_codes.begin(), snapshot.reason_codes.end(),
      [](const std::string &reason) {
        return reason == "trajectory_controller_duplicated" ||
               reason == "trajectory_controller_type_mismatch" ||
               reason == "trajectory_controller_claims_mismatch" ||
               reason == "trajectory_controller_claim_conflict" ||
               reason == "hardware_component_duplicated" ||
               reason == "hardware_command_interfaces_unavailable" ||
               reason == "hardware_state_interfaces_unavailable";
      });
}

[[nodiscard]] std::string binding_signature(
    const std::optional<ros2_control_execution::ControllerBindingSnapshot>
        &snapshot) {
  if (!snapshot.has_value()) {
    return "binding:absent";
  }
  std::ostringstream stream;
  stream << "binding:" << snapshot->generation << ':'
         << snapshot->activity_generation << ':'
         << snapshot->contract_generation_id << ':'
         << snapshot->observed_timestamp_ns << ':'
         << snapshot->received_timestamp_ns << ':' << snapshot->fresh_until_ns
         << ':' << snapshot->controller_active << ':'
         << snapshot->hardware_active << ':'
         << snapshot->controller_type_matches << ':'
         << snapshot->claimed_interfaces_match << ':'
         << snapshot->state_interfaces_available << ':'
         << snapshot->conflicting_claims_absent << ':' << snapshot->valid;
  for (const auto &reason : snapshot->reason_codes) {
    stream << ':' << reason;
  }
  return stream.str();
}

[[nodiscard]] std::string gripper_signature(
    const ros2_control_execution::Ros2GripperTransportReadinessSnapshot
        &snapshot) {
  std::ostringstream stream;
  stream << "gripper:" << snapshot.transport_kind << ':' << snapshot.configured
         << ':' << snapshot.command_path_ready << ':'
         << snapshot.feedback_path_ready << ':' << snapshot.command_gate_open
         << ':' << snapshot.ready;
  for (const auto &reason : snapshot.reason_codes) {
    stream << ':' << reason;
  }
  return stream.str();
}

} // namespace

core::Result<void> RetainedRos2ControlRuntimeHandle::BindBackend(
    std::shared_ptr<ros2_control_execution::Ros2ControlExecutionBackend>
        backend,
    const core::RuntimeParticipantBinding &motion_binding,
    const std::optional<core::RuntimeParticipantBinding> &gripper_binding) {
  if (!backend || !motion_binding.value().control_path.has_value() ||
      (gripper_binding.has_value() &&
       !gripper_binding->value().control_path.has_value())) {
    return reject<void>("ros2_control_handle_binding_invalid",
                        "ROS runtime handle requires exact control-addressed "
                        "physical bindings");
  }
  const auto &motion_domain =
      motion_binding.value().control_path->control_domain_id;
  const auto gripper_domain =
      gripper_binding.has_value()
          ? std::optional(
                gripper_binding->value().control_path->control_domain_id)
          : std::nullopt;
  if (gripper_domain.has_value() && *gripper_domain == motion_domain) {
    return reject<void>("ros2_control_handle_domain_alias",
                        "motion and gripper bindings cannot alias one control "
                        "domain");
  }

  std::lock_guard lock(mutex_);
  if (!backend_.expired() || motion_domain_id_.has_value()) {
    return reject<void>("ros2_control_handle_already_bound",
                        "ROS runtime handle is already bound");
  }
  backend_ = std::move(backend);
  motion_domain_id_ = motion_domain;
  gripper_domain_id_ = gripper_domain;
  if (gripper_binding.has_value()) {
    gripper_id_ = gripper_binding->value().command_target_id.value();
  }
  return core::Result<void>::Ok();
}

RetainedExternallyManagedDomainSnapshotSource
RetainedRos2ControlRuntimeHandle::lifecycle_snapshot_source() {
  const auto self = shared_from_this();
  return [self](const core::ControlDomainId &domain_id,
                const core::TimePoint now_ns) {
    return self->ReadLifecycleSnapshot(domain_id, now_ns);
  };
}

core::Result<RetainedExternallyManagedDomainSnapshot>
RetainedRos2ControlRuntimeHandle::ReadLifecycleSnapshot(
    const core::ControlDomainId &control_domain_id,
    const core::TimePoint now_ns) {
  std::shared_ptr<ros2_control_execution::Ros2ControlExecutionBackend> backend;
  std::optional<core::ControlDomainId> motion_domain;
  std::optional<core::ControlDomainId> gripper_domain;
  std::optional<std::string> gripper_id;
  {
    std::lock_guard lock(mutex_);
    backend = backend_.lock();
    motion_domain = motion_domain_id_;
    gripper_domain = gripper_domain_id_;
    gripper_id = gripper_id_;
  }
  if (!backend || now_ns < 0 || control_domain_id.empty() ||
      (!motion_domain.has_value() && !gripper_domain.has_value())) {
    return reject<RetainedExternallyManagedDomainSnapshot>(
        "ros2_control_lifecycle_source_unavailable",
        "ROS runtime lifecycle source is unbound or unavailable");
  }

  core::ComponentLifecycleStateDTO state =
      core::ComponentLifecycleStateDTO::kDegraded;
  bool exact_configuration_ready = false;
  bool fault_present = false;
  bool transport_ready = false;
  std::string signature;
  if (motion_domain.has_value() && control_domain_id == *motion_domain) {
    const auto snapshot = backend->controller_binding_snapshot();
    signature = binding_signature(snapshot);
    if (snapshot.has_value()) {
      const bool fresh = snapshot->received_timestamp_ns <= now_ns &&
                         now_ns < snapshot->fresh_until_ns;
      transport_ready = fresh;
      exact_configuration_ready = fresh && snapshot->valid;
      fault_present = fresh && binding_fault(*snapshot);
      if (exact_configuration_ready) {
        state = core::ComponentLifecycleStateDTO::kActive;
      } else if (fault_present) {
        state = core::ComponentLifecycleStateDTO::kFaulted;
      } else if (fresh && !snapshot->controller_active &&
                 !snapshot->hardware_active) {
        state = core::ComponentLifecycleStateDTO::kDisabled;
      }
    }
  } else if (gripper_domain.has_value() && gripper_id.has_value() &&
             control_domain_id == *gripper_domain) {
    const auto snapshot = backend->gripper_transport_readiness(*gripper_id);
    signature = gripper_signature(snapshot);
    transport_ready = snapshot.ready;
    exact_configuration_ready = snapshot.ready;
    fault_present = !snapshot.configured;
    state = snapshot.ready  ? core::ComponentLifecycleStateDTO::kActive
            : fault_present ? core::ComponentLifecycleStateDTO::kFaulted
                            : core::ComponentLifecycleStateDTO::kDegraded;
  } else {
    return reject<RetainedExternallyManagedDomainSnapshot>(
        "ros2_control_lifecycle_domain_unknown",
        "requested lifecycle domain is not bound to this ROS runtime");
  }

  std::lock_guard lock(mutex_);
  auto &generation = domain_generations_[control_domain_id];
  if (generation.publication_generation == 0U) {
    generation.publication_generation = 1U;
    generation.source_signature = std::move(signature);
  } else if (generation.source_signature != signature) {
    if (generation.publication_generation ==
        std::numeric_limits<std::uint64_t>::max()) {
      return reject<RetainedExternallyManagedDomainSnapshot>(
          "ros2_control_lifecycle_generation_exhausted",
          "ROS lifecycle observation generation is exhausted");
    }
    ++generation.publication_generation;
    generation.source_signature = std::move(signature);
  }
  RetainedExternallyManagedDomainSnapshot result{
      generation.publication_generation, state, exact_configuration_ready,
      lifecycle_facts(exact_configuration_ready, fault_present,
                      transport_ready)};
  if (!result.locally_valid()) {
    return reject<RetainedExternallyManagedDomainSnapshot>(
        "ros2_control_lifecycle_snapshot_invalid",
        "ROS lifecycle normalization produced an invalid snapshot");
  }
  return core::Result<RetainedExternallyManagedDomainSnapshot>::Ok(
      std::move(result));
}

bool RetainedRos2ControlRuntimeHandle::publish_execution_admission_snapshot(
    ros2_control_execution::ExecutionAdmissionSnapshot snapshot) {
  std::shared_ptr<ros2_control_execution::Ros2ControlExecutionBackend> backend;
  {
    std::lock_guard lock(mutex_);
    backend = backend_.lock();
  }
  return backend &&
         backend->publish_execution_admission_snapshot(std::move(snapshot));
}

std::optional<ros2_control_execution::ExecutionAdmissionSnapshot>
RetainedRos2ControlRuntimeHandle::execution_admission_snapshot() const {
  std::shared_ptr<ros2_control_execution::Ros2ControlExecutionBackend> backend;
  {
    std::lock_guard lock(mutex_);
    backend = backend_.lock();
  }
  return backend ? backend->execution_admission_snapshot() : std::nullopt;
}

std::optional<ros2_control_execution::ControllerBindingSnapshot>
RetainedRos2ControlRuntimeHandle::controller_binding_snapshot() const {
  std::shared_ptr<ros2_control_execution::Ros2ControlExecutionBackend> backend;
  {
    std::lock_guard lock(mutex_);
    backend = backend_.lock();
  }
  return backend ? backend->controller_binding_snapshot() : std::nullopt;
}

} // namespace manyforge::roboplan_backend
