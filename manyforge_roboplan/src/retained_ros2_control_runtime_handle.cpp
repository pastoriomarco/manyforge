#include <manyforge_roboplan/retained_ros2_control_runtime_handle.hpp>

#include <algorithm>
#include <initializer_list>
#include <limits>
#include <sstream>
#include <string_view>
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

[[nodiscard]] core::LifecycleFactDTO
cause_fact(const core::LifecycleBlockingCauseClassDTO cause) {
  return {core::LifecycleFactId(
              std::string(core::kLifecycleBlockingCauseFactId)),
          {core::LifecycleFactValueKindDTO::kToken, std::nullopt, std::nullopt,
           std::string(core::to_string(cause))}};
}

// Fact ids are sorted: every `external.*` id precedes the standard
// `manyforge.blocking_cause_class` id.
[[nodiscard]] std::vector<core::LifecycleFactDTO>
lifecycle_facts(const bool controller_ready,
                const bool controller_inactive_unrequested,
                const bool fault_present, const bool transport_ready,
                const core::LifecycleBlockingCauseClassDTO cause) {
  return {
      boolean_fact("external.controller_configuration_ready", controller_ready),
      // Diagnostic only: fresh status shows the trajectory controller inactive
      // or missing after the exact configuration was active. The cause stays
      // `availability`; CCLC turns it into `provider_lost` after Active.
      boolean_fact("external.controller_inactive_unrequested",
                   controller_inactive_unrequested),
      boolean_fact("external.fault_present", fault_present),
      boolean_fact("external.transport_ready", transport_ready),
      cause_fact(cause),
  };
}

[[nodiscard]] bool
any_reason(const std::vector<std::string> &reasons,
           const std::initializer_list<std::string_view> candidates) {
  return std::any_of(reasons.begin(), reasons.end(),
                     [&](const std::string &reason) {
                       return std::find(candidates.begin(), candidates.end(),
                                        reason) != candidates.end();
                     });
}

[[nodiscard]] bool
only_reasons(const std::vector<std::string> &reasons,
             const std::initializer_list<std::string_view> candidates) {
  return !reasons.empty() &&
         std::all_of(reasons.begin(), reasons.end(),
                     [&](const std::string &reason) {
                       return std::find(candidates.begin(), candidates.end(),
                                        reason) != candidates.end();
                     });
}

// Classifies one fresh controller-manager binding snapshot. ros2_control
// reports no emergency/protective/safeguard stop status, so those classes are
// never claimed here. A deactivated hardware component may be a hardware error
// as well as a deliberate deactivation, so it stays `unknown`.
[[nodiscard]] core::LifecycleBlockingCauseClassDTO motion_binding_cause(
    const ros2_control_execution::ControllerBindingSnapshot &snapshot) {
  using Cause = core::LifecycleBlockingCauseClassDTO;
  const auto &reasons = snapshot.reason_codes;
  if (snapshot.valid && reasons.empty()) {
    return Cause::kNone;
  }
  if (any_reason(reasons, {"hardware_command_interfaces_unavailable",
                           "hardware_state_interfaces_unavailable"})) {
    return Cause::kFault;
  }
  if (any_reason(reasons, {"trajectory_controller_duplicated",
                           "trajectory_controller_type_mismatch",
                           "trajectory_controller_claims_mismatch",
                           "trajectory_controller_claim_conflict",
                           "hardware_component_duplicated"})) {
    return Cause::kConfiguration;
  }
  if (only_reasons(reasons, {"controller_activity_changed",
                             "controller_activity_unobserved",
                             "controller_binding_monitor_unavailable",
                             "controller_list_service_unavailable",
                             "controller_list_service_timeout",
                             "hardware_list_service_unavailable",
                             "hardware_list_service_timeout",
                             "trajectory_controller_missing",
                             "trajectory_controller_inactive",
                             "hardware_component_missing"})) {
    return Cause::kAvailability;
  }
  return Cause::kUnknown;
}

[[nodiscard]] core::LifecycleBlockingCauseClassDTO gripper_transport_cause(
    const ros2_control_execution::Ros2GripperTransportReadinessSnapshot
        &snapshot) {
  using Cause = core::LifecycleBlockingCauseClassDTO;
  const auto &reasons = snapshot.reason_codes;
  if (snapshot.ready) {
    return Cause::kNone;
  }
  if (any_reason(reasons, {"gripper_identity_missing",
                           "gripper_transport_not_configured",
                           "gripper_transport_kind_invalid"})) {
    return Cause::kConfiguration;
  }
  if (only_reasons(reasons, {"gripper_action_server_unavailable",
                             "gripper_command_subscriber_unavailable",
                             "gripper_state_feedback_unavailable",
                             "gripper_state_publisher_unavailable",
                             "gripper_state_message_unreceived",
                             "gripper_state_joint_unobserved",
                             "gripper_state_feedback_stale",
                             "gripper_transport_replaced"})) {
    return Cause::kAvailability;
  }
  return Cause::kUnknown;
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
         << snapshot.feedback_path_ready << ':' << snapshot.ready;
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
  motion_binding_ = motion_binding;
  gripper_binding_ = gripper_binding;
  motion_domain_id_ = motion_domain;
  gripper_domain_id_ = gripper_domain;
  if (gripper_binding.has_value()) {
    gripper_id_ = gripper_binding->value().command_target_id.value();
  }
  return core::Result<void>::Ok();
}

bool RetainedRos2ControlRuntimeHandle::OwnsBackend(
    const core::IExecutionBackend *backend,
    const core::RuntimeParticipantBinding &motion_binding,
    const std::optional<core::RuntimeParticipantBinding> &gripper_binding)
    const {
  std::lock_guard lock(mutex_);
  const auto owned = backend_.lock();
  return owned != nullptr && owned.get() == backend &&
         motion_binding_.has_value() &&
         motion_binding_->value() == motion_binding.value() &&
         gripper_binding_.has_value() == gripper_binding.has_value() &&
         (!gripper_binding.has_value() ||
          gripper_binding_->value() == gripper_binding->value());
}

void RetainedRos2ControlRuntimeHandle::StartBackend() {
  std::lock_guard lock(mutex_);
  if (start_failure_ != nullptr)
    std::rethrow_exception(start_failure_);
  if (start_attempted_)
    return;
  const auto backend = backend_.lock();
  if (backend == nullptr)
    throw std::logic_error("ROS initial installation has no live backend");
  start_attempted_ = true;
  try {
    backend->complete_ros_configuration();
  } catch (...) {
    start_failure_ = std::current_exception();
    throw;
  }
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
  bool controller_inactive = false;
  // Stale or missing status is an availability loss unless fresh status
  // previously established an intervention-requiring cause (resolved below).
  core::LifecycleBlockingCauseClassDTO observed_cause =
      core::LifecycleBlockingCauseClassDTO::kAvailability;
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
      if (fresh) {
        observed_cause = motion_binding_cause(*snapshot);
        controller_inactive = !snapshot->controller_active;
      }
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
    observed_cause = gripper_transport_cause(snapshot);
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
  // An availability observation cannot clear an intervention-requiring cause;
  // any other classification is fresh status and replaces it.
  const auto cause =
      observed_cause == core::LifecycleBlockingCauseClassDTO::kAvailability &&
              generation.last_intervention_cause !=
                  core::LifecycleBlockingCauseClassDTO::kNone
          ? generation.last_intervention_cause
          : observed_cause;
  if (observed_cause != core::LifecycleBlockingCauseClassDTO::kAvailability) {
    generation.last_intervention_cause =
        core::lifecycle_cause_requires_intervention(observed_cause)
            ? observed_cause
            : core::LifecycleBlockingCauseClassDTO::kNone;
  }
  generation.exact_configuration_seen =
      generation.exact_configuration_seen || exact_configuration_ready;
  const bool controller_inactive_unrequested =
      controller_inactive && generation.exact_configuration_seen;
  signature += ":cause:";
  signature += core::to_string(cause);
  signature += controller_inactive_unrequested ? ":inactive_unrequested" : "";
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
      lifecycle_facts(exact_configuration_ready,
                      controller_inactive_unrequested, fault_present,
                      transport_ready, cause)};
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

std::optional<ros2_control_execution::Ros2GripperTransportReadinessSnapshot>
RetainedRos2ControlRuntimeHandle::gripper_transport_readiness_snapshot() const {
  std::shared_ptr<ros2_control_execution::Ros2ControlExecutionBackend> backend;
  std::optional<std::string> gripper_id;
  {
    std::lock_guard lock(mutex_);
    backend = backend_.lock();
    gripper_id = gripper_id_;
  }
  if (!backend || !gripper_id.has_value()) {
    return std::nullopt;
  }
  return backend->gripper_transport_readiness(*gripper_id);
}

} // namespace manyforge::roboplan_backend
