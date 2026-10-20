#include <manyforge_core/io_signal_descriptor.hpp>
#include "execution_host_surface.hpp"
#if defined(MANYFORGE_ROS2_CONTROL_EXECUTION_HOST)
#include <manyforge_ros2_control_execution/volatile_motion_trace.hpp>
#endif
#include "execution_host_responses.hpp"
#include "cell_admission_command_codec.hpp"
#include "sealed_program_execution_capture.hpp"
#include "host_io_signal_runtime.hpp"
#include <manyforge_core/component_lifecycle_value_codec.hpp>
#include <manyforge_core/types/lifecycle_cause.hpp>
#include <manyforge_deployment/program_binding.hpp>
#include <manyforge_deployment/program_binding_compiler.hpp>
#include <manyforge_deployment/io_signal_catalog.hpp>
#include <manyforge_deployment/runtime_program_binding.hpp>
#include <fcntl.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>
#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <climits>
#include <cstring>
#include <cstdlib>
#include <stdexcept>
#include <tuple>
#include <iostream>
#include <set>
#include <thread>

namespace manyforge::roboplan_backend::host_detail {

namespace {

[[nodiscard]] bool IsQualifiedProviderOnlyView(const Composition &owner) {
  return !owner.runtime && !owner.gripper_runtime &&
         !owner.direct_control_motion_binding &&
         !owner.direct_control_gripper_binding && owner.bundle &&
         owner.io_provider_drivers && !owner.io_provider_drivers->empty() &&
         owner.component_runtime_pins &&
         owner.component_runtime_pins->matches(
             owner.runtime, owner.gripper_runtime, owner.io_provider_drivers) &&
         IsProviderOnlyNativeComponent(owner.bundle->value().cell_graph,
                                       owner.controllable_component_id);
}

class CollisionLeaseRelease final {
public:
  CollisionLeaseRelease(core::ICollisionWorldSnapshotPublisher &publisher,
                        core::ProjectionLeaseId lease_id)
      : publisher_(publisher), lease_id_(std::move(lease_id)) {}
  ~CollisionLeaseRelease() {
    publisher_.release_scene_snapshot_lease_inventory(lease_id_);
  }
  CollisionLeaseRelease(const CollisionLeaseRelease &) = delete;
  CollisionLeaseRelease &operator=(const CollisionLeaseRelease &) = delete;

private:
  core::ICollisionWorldSnapshotPublisher &publisher_;
  core::ProjectionLeaseId lease_id_;
};

/// Admission check for robot-qualified references: each portable target
/// role must resolve through the operation's exact Program Binding to the
/// named controllable component and command target. Locality is checked by the
/// cell frame resolver when the reference is captured.
[[nodiscard]] core::Result<void>
VerifyReferenceTarget(const ProductBackedRuntimeParticipantResolver &resolver,
                      const core::ProgramBindingId &program_binding_id,
                      const core::ReferenceDTO &reference) {
  if (!reference.target.has_value())
    return core::Result<void>::Ok();
  const auto &target = *reference.target;
  const auto bound =
      resolver.ResolveOperationTarget(program_binding_id, target.target_role_id);
  // Both refusals name the failed reference (spec 210 §5.3).
  const std::string frame_role_id(core::reference_identity_value(reference));
  if (!bound.ok())
    return core::Result<void>::Err(core::ErrorDTO{
        std::string(core::to_string(core::ErrorCode::kMissingReference)),
        "frame reference names a target role absent from the Program Binding",
        {{"reason", "program_reference_target_role_unbound"},
         {"reference_type", "frame_role"},
         {"reference_id", frame_role_id},
         {"target_role_id", target.target_role_id.value()}}});
  if (bound.value().controllable_component_id !=
          target.controllable_component_id ||
      bound.value().command_target_id != target.command_target_id)
    return core::Result<void>::Err(core::ErrorDTO{
        core::kErrorInvalidRequest,
        "frame reference robot differs from its Program Binding target role",
        {{"reason", "program_reference_target_role_mismatch"},
         {"reference_type", "frame_role"},
         {"reference_id", frame_role_id},
         {"target_role_id", target.target_role_id.value()}}});
  return core::Result<void>::Ok();
}

[[nodiscard]] core::Result<void>
VerifyReferenceTargets(const ProductBackedRuntimeParticipantResolver &resolver,
                       const core::OperationIntentDTO &operation) {
  for (const auto &participant : operation.participants) {
    const auto *motion =
        std::get_if<core::MotionParticipantIntentDTO>(&participant.payload);
    if (motion == nullptr)
      continue;
    for (const auto *reference :
         {&motion->target_reference, &motion->offset_reference,
          &motion->goal_frame_reference}) {
      if (!reference->has_value())
        continue;
      if (auto verified = VerifyReferenceTarget(
              resolver, operation.program_binding_id, **reference);
          !verified.ok())
        return verified;
    }
  }
  return core::Result<void>::Ok();
}

/// Pick and approach offset references accept every motion-reference form
/// (spec 475 §3.5); a robot-qualified one passes the same Program-Binding
/// check against the picking motion's binding as a motion reference.
[[nodiscard]] core::Result<void>
VerifyPickReferenceTargets(const ProductBackedRuntimeParticipantResolver &resolver,
                           const core::PickSequencePlanningIntentDTO &intent) {
  for (const auto *operation :
       {&intent.approach_operation, &intent.descend_operation,
        &intent.tool_operation, &intent.retreat_operation}) {
    if (auto verified = VerifyReferenceTargets(resolver, *operation);
        !verified.ok())
      return verified;
  }
  for (const auto *reference : {&intent.synthesis.pick_offset_reference,
                                &intent.synthesis.approach_offset_reference}) {
    if (!reference->has_value())
      continue;
    if (auto verified =
            VerifyReferenceTarget(resolver,
                                  intent.approach_operation.program_binding_id,
                                  **reference);
        !verified.ok())
      return verified;
  }
  return core::Result<void>::Ok();
}


} // namespace

ManagedExecutionSurfaceControl::ManagedExecutionSurfaceControl(
      std::filesystem::path selected_run_root,
      ExecutionCompositionAccess *selected_composition_access,
      component_manager::detail::ManagerLivenessCommandAdmission
          *selected_manager_liveness_admission,
      core::RequestOwnerEpoch selected_owner_epoch)
      : run_root(std::move(selected_run_root)),
        composition_access(selected_composition_access),
        manager_liveness_admission(selected_manager_liveness_admission),
        owner_epoch(std::move(selected_owner_epoch)) {}

ExecutionCommandSurface::ExecutionCommandSurface(
      std::filesystem::path run_root,
      ExecutionCompositionAccess &composition_access,
      component_manager::detail::ManagerLivenessCommandAdmission
          &manager_liveness_admission,
      core::RequestOwnerEpoch owner_epoch)
      : owner_epoch_(std::move(owner_epoch)),
        direct_control_program_run_id_("direct-control-" +
                                       owner_epoch_.value()),
        direct_control_gripper_program_run_id_("direct-control-gripper-" +
                                               owner_epoch_.value()),
        composition_access_(composition_access),
        manager_liveness_admission_(manager_liveness_admission),
        host_lock_(run_root), command_socket_(run_root, kSocketFileName),
        control_socket_(run_root, kControlSocketFileName),
        observation_socket_(run_root, kObservationSocketFileName) {
    if (!lane_wake_) {
      FailErrno("create execution lane wake");
    }
  }

int ExecutionCommandSurface::Run(const std::stop_token stop_token) {
    const std::stop_callback wake_on_stop(stop_token, [this] { WakeLanes(); });
    std::jthread control_thread(
        [this](std::stop_token stop_token) { RunControlLane(stop_token); });
    std::jthread observation_thread(
        [this](std::stop_token stop_token) { RunObservationLane(stop_token); });
    std::jthread reconciliation_thread([this](std::stop_token stop_token) {
      RunReconciliationLane(stop_token);
    });
    std::jthread submission_ordered_thread(
        [this](std::stop_token stop_token) {
          RunDeferredLane(submission_ordered_lane_, stop_token);
        });
    std::jthread scene_read_thread([this](std::stop_token stop_token) {
      RunDeferredLane(scene_read_lane_, stop_token);
    });
    while (g_stop_requested == 0 && !stop_token.stop_requested() &&
           !reconciliation_lane_failed_.load(std::memory_order_acquire)) {
      short revents = 0;
      int ready = -1;
      do {
        ready = WaitForSocket(command_socket_.get(), revents);
      } while (ready < 0 && errno == EINTR && g_stop_requested == 0 &&
               !stop_token.stop_requested() &&
               !reconciliation_lane_failed_.load(std::memory_order_acquire));
      if (g_stop_requested != 0 || stop_token.stop_requested() ||
          reconciliation_lane_failed_.load(std::memory_order_acquire)) {
        break;
      }
      if (ready < 0) {
        FailErrno("poll execution command socket");
      }
      if ((revents & POLLIN) != 0) {
        HandleOneClient(command_socket_, CommandLane::kOrdinary, stop_token);
      }
      if ((revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
        Fail("execution command socket became unavailable");
      }
      if (control_socket_failed_.load(std::memory_order_acquire)) {
        Fail("execution Stop control socket became unavailable");
      }
    }
    reconciliation_thread.request_stop();
    control_thread.request_stop();
    observation_thread.request_stop();
    submission_ordered_thread.request_stop();
    scene_read_thread.request_stop();
    submission_ordered_thread.join();
    scene_read_thread.join();
    reconciliation_thread.join();
    control_thread.join();
    observation_thread.join();
    if (reconciliation_lane_failed_.load(std::memory_order_acquire)) {
      std::exception_ptr failure;
      {
        std::lock_guard lock(reconcile_error_mutex_);
        failure = reconciliation_lane_failure_;
      }
      Require(failure != nullptr,
              "execution reconciliation lane failed without an exception");
      std::rethrow_exception(failure);
    }
    Require(!control_socket_failed_.load(std::memory_order_acquire),
            "execution Stop control socket became unavailable");
    return 0;
  }

#if defined(MANYFORGE_ROS2_CONTROL_EXECUTION_HOST)
  [[nodiscard]] CanonicalValue
  ExecutionCommandSurface::Ros2ControlLifecycleObservationValue(Composition &composition) const {
    Require(composition.ros2_control_runtime_handle != nullptr &&
                composition.runtime != nullptr &&
                composition.runtime->clock != nullptr,
            "ROS lifecycle status has no selected observer or clock");

    std::vector<core::ControlDomainId> domain_ids;
    domain_ids.reserve(2U);
    std::vector<const core::RuntimeParticipantBinding *> bindings{
        &*composition.direct_control_motion_binding};
    if (composition.direct_control_gripper_binding.has_value())
      bindings.push_back(&*composition.direct_control_gripper_binding);
    for (const auto *binding : bindings) {
      Require(binding->value().control_path.has_value(),
              "ROS lifecycle status binding has no retained control path");
      domain_ids.push_back(binding->value().control_path->control_domain_id);
    }
    std::sort(domain_ids.begin(), domain_ids.end());
    domain_ids.erase(std::unique(domain_ids.begin(), domain_ids.end()),
                     domain_ids.end());
    Require(!domain_ids.empty(),
            "ROS lifecycle status has no retained control domain");

    const auto now_ns = composition.runtime->clock->now();
    bool every_domain_ready = true;
    CanonicalValue::Array domains;
    domains.reserve(domain_ids.size());
    for (const auto &domain_id : domain_ids) {
      const auto sampled =
          composition.ros2_control_runtime_handle->ReadLifecycleSnapshot(
              domain_id, now_ns);
      if (!sampled.ok()) {
        Fail("ROS lifecycle status observation failed: " +
             sampled.error().message);
      }
      const auto &observation = sampled.value();
      Require(observation.locally_valid(),
              "ROS lifecycle status observation is malformed");

      std::optional<bool> controller_configuration_ready;
      std::optional<bool> fault_present;
      std::optional<bool> transport_ready;
      std::optional<core::LifecycleBlockingCauseClassDTO> blocking_cause;
      CanonicalValue::Array facts;
      facts.reserve(observation.facts.size());
      for (const auto &fact : observation.facts) {
        // The standard cause-class token is reported beside, not inside, the
        // retained boolean readiness facts of this status schema.
        if (fact.lifecycle_fact_id.value() ==
            core::kLifecycleBlockingCauseFactId) {
          Require(fact.locally_valid() &&
                      fact.value.kind ==
                          core::LifecycleFactValueKindDTO::kToken &&
                      fact.value.token_value.has_value() &&
                      !blocking_cause.has_value(),
                  "ROS lifecycle cause fact is not one exact token");
          blocking_cause = core::parse_lifecycle_blocking_cause_class(
              *fact.value.token_value);
          Require(blocking_cause.has_value(),
                  "ROS lifecycle cause fact names no known cause class");
          continue;
        }
        Require(fact.locally_valid() &&
                    fact.value.kind ==
                        core::LifecycleFactValueKindDTO::kBoolean &&
                    fact.value.boolean_value.has_value(),
                "ROS lifecycle status fact is not one exact boolean");
        const auto &fact_id = fact.lifecycle_fact_id.value();
        const bool value = *fact.value.boolean_value;
        if (fact_id == "external.controller_configuration_ready") {
          controller_configuration_ready = value;
        } else if (fact_id == "external.fault_present") {
          fault_present = value;
        } else if (fact_id == "external.transport_ready") {
          transport_ready = value;
        }
        facts.emplace_back(CanonicalValue::Object{
            {"lifecycle_fact_id", CanonicalValue(fact_id)},
            {"value", CanonicalValue(value)},
        });
      }
      Require(controller_configuration_ready.has_value() &&
                  fault_present.has_value() && transport_ready.has_value(),
              "ROS lifecycle status omits one retained readiness fact");
      const bool ready = observation.component_state ==
                             core::ComponentLifecycleStateDTO::kActive &&
                         observation.exact_controller_configuration_active &&
                         *controller_configuration_ready && !*fault_present &&
                         *transport_ready;
      every_domain_ready = every_domain_ready && ready;
      domains.emplace_back(CanonicalValue::Object{
          {"blocking_cause_class",
           CanonicalValue(std::string(core::to_string(blocking_cause.value_or(
               core::LifecycleBlockingCauseClassDTO::kUnknown))))},
          {"component_lifecycle_state",
           CanonicalValue(
               std::string(core::to_string(observation.component_state)))},
          {"control_domain_id", CanonicalValue(domain_id.value())},
          {"exact_controller_configuration_active",
           CanonicalValue(observation.exact_controller_configuration_active)},
          {"facts", CanonicalValue(std::move(facts))},
          {"ready", CanonicalValue(ready)},
          {"source_generation",
           CanonicalValue(static_cast<double>(observation.source_generation))},
      });
    }
    return CanonicalValue(CanonicalValue::Object{
        {"domains", CanonicalValue(std::move(domains))},
        {"ready", CanonicalValue(every_domain_ready)},
        {"schema_version",
         CanonicalValue("manyforge.ros2_control_lifecycle_observation.v1")},
    });
  }

[[nodiscard]] CanonicalValue
  ExecutionCommandSurface::Ros2ControlTransportDiagnosticValue(Composition &composition) const {
    Require(composition.ros2_control_runtime_handle != nullptr,
            "ROS transport diagnostics have no selected observer");
    const auto sampled = composition.ros2_control_runtime_handle
                             ->gripper_transport_readiness_snapshot();
    Require(sampled.has_value(),
            "ROS transport diagnostics have no bound gripper transport");
    const auto &snapshot = *sampled;
    constexpr std::uint64_t kMaximumSafeUnsignedInteger =
        9'007'199'254'740'991ULL;
    Require(snapshot.feedback_publisher_count <= kMaximumSafeUnsignedInteger &&
                snapshot.feedback_messages_received <=
                    kMaximumSafeUnsignedInteger &&
                snapshot.feedback_samples_matched <=
                    kMaximumSafeUnsignedInteger,
            "ROS transport diagnostic counter exceeds JSON safe integer");

    CanonicalValue::Array reason_codes;
    reason_codes.reserve(snapshot.reason_codes.size());
    for (const auto &reason : snapshot.reason_codes) {
      Require(!reason.empty() && reason.size() <= kMaximumIdentityBytes,
              "ROS transport diagnostic reason is not bounded");
      reason_codes.emplace_back(CanonicalValue(reason));
    }
    return CanonicalValue(CanonicalValue::Object{
        {"command_path_ready", CanonicalValue(snapshot.command_path_ready)},
        {"configured", CanonicalValue(snapshot.configured)},
        {"feedback_messages_received",
         CanonicalValue(
             static_cast<double>(snapshot.feedback_messages_received))},
        {"feedback_path_ready", CanonicalValue(snapshot.feedback_path_ready)},
        {"feedback_publisher_count", CanonicalValue(static_cast<double>(
                                         snapshot.feedback_publisher_count))},
        {"feedback_samples_matched", CanonicalValue(static_cast<double>(
                                         snapshot.feedback_samples_matched))},
        {"ready", CanonicalValue(snapshot.ready)},
        {"reason_codes", CanonicalValue(std::move(reason_codes))},
        {"schema_version",
         CanonicalValue("manyforge.ros2_control_transport_diagnostic.v1")},
        {"state_joint_name", CanonicalValue(snapshot.state_joint_name)},
        {"state_topic", CanonicalValue(snapshot.state_topic)},
        {"transport_kind", CanonicalValue(snapshot.transport_kind)},
    });
  }

#endif

  [[nodiscard]] CanonicalValue
  ExecutionCommandSurface::DirectControlTargetValue(Composition &composition) const {
    const auto &binding = composition.direct_control_motion_binding->value();
    const auto &limits = *composition.direct_control_joint_limits;
    Require(
        limits.joint_names.size() == binding.resources.size() &&
            limits.position_lower_rad.size() == binding.resources.size() &&
            limits.position_upper_rad.size() == binding.resources.size() &&
            limits.velocity_max_rad_s.size() == binding.resources.size() &&
            (limits.acceleration_max_rad_s2.empty() ||
             limits.acceleration_max_rad_s2.size() == binding.resources.size()),
        "direct-control joint limits cross their retained target");

    CanonicalValue::Array joints;
    joints.reserve(binding.resources.size());
    for (std::size_t index = 0U; index < binding.resources.size(); ++index) {
      const auto &resource = binding.resources[index];
      Require(resource.source_joint_name.has_value() &&
                  resource.coordinate_kind == core::CoordinateKind::kAngular &&
                  limits.joint_names[index] == resource.compiled_resource_name,
              "direct-control joint limits cross their source-local binding");
      joints.emplace_back(CanonicalValue::Object{
          {"max_acceleration_rad_s2",
           limits.acceleration_max_rad_s2.empty()
               ? CanonicalValue(nullptr)
               : CanonicalValue(limits.acceleration_max_rad_s2[index])},
          {"max_position_rad",
           CanonicalValue(limits.position_upper_rad[index])},
          {"max_velocity_rad_s",
           CanonicalValue(limits.velocity_max_rad_s[index])},
          {"min_position_rad",
           CanonicalValue(limits.position_lower_rad[index])},
          {"source_joint_name", CanonicalValue(*resource.source_joint_name)},
      });
    }
    CanonicalValue::Array frame_roles;
    frame_roles.reserve(binding.frame_bindings.size());
    for (const auto &frame : binding.frame_bindings) {
      frame_roles.emplace_back(frame.frame_role_id.value());
    }
    CanonicalValue::Array planning_capabilities;
    for (const auto &path : binding.supporting_capability_provider_paths) {
      planning_capabilities.emplace_back(path.capability_id.value());
    }
    const auto base_frame =
        std::find_if(binding.frame_bindings.begin(),
                     binding.frame_bindings.end(), [](const auto &frame) {
                       return frame.frame_role_id.value() ==
                              deployment::kDirectControlBaseFrameRoleId;
                     });
    const auto tcp_frame =
        std::find_if(binding.frame_bindings.begin(),
                     binding.frame_bindings.end(), [](const auto &frame) {
                       return frame.frame_role_id.value() ==
                              deployment::kDirectControlTcpFrameRoleId;
                     });
    Require(base_frame != binding.frame_bindings.end() &&
                tcp_frame != binding.frame_bindings.end(),
            "direct-control status has no exact base/TCP frame binding");
    return CanonicalValue(CanonicalValue::Object{
        {"command_target_id",
         CanonicalValue(binding.command_target_id.value())},
        {"base_frame_role_id", CanonicalValue(std::string(
                                   deployment::kDirectControlBaseFrameRoleId))},
        {"component_role_id",
         CanonicalValue(binding.component_role_id.value())},
        {"controllable_component_id",
         CanonicalValue(binding.controllable_component_id.value())},
        {"frame_role_ids", CanonicalValue(std::move(frame_roles))},
        {"joints", CanonicalValue(std::move(joints))},
        {"operation_capability_id",
         CanonicalValue(binding.capability_id.value())},
        {"planning_capability_ids",
         CanonicalValue(std::move(planning_capabilities))},
        {"program_binding_id",
         CanonicalValue(binding.program_binding_id.value())},
        {"program_id", CanonicalValue(binding.program_id.value())},
        {"program_revision", CanonicalValue(binding.program_revision.value())},
        {"program_run_id",
         CanonicalValue(direct_control_program_run_id_.value())},
        {"target_role_id", CanonicalValue(binding.target_role_id.value())},
        {"target_descriptor", composition.direct_control_motion_descriptor},
        {"tcp_frame_role_id",
         CanonicalValue(std::string(deployment::kDirectControlTcpFrameRoleId))},
    });
  }

[[nodiscard]] std::optional<ExecutionCommandSurface::ResolvedObservationOwner>
  ExecutionCommandSurface::ResolveObservationOwner(const core::ProgramBindingId &program_binding_id,
                          const core::TargetRoleId &target_role_id,
                          const core::CapabilityId &capability_id,
                          const bool motion, Composition &composition,
                          std::map<std::string, std::string> *failures) const {
    std::optional<ResolvedObservationOwner> match;
    bool ambiguous = false;
    const auto inspect = [&](Composition &candidate) {
      const auto refuse = [&](std::string phase, const core::ErrorDTO *error = nullptr) {
        if (failures == nullptr)
          return;
        const auto prefix = candidate.controllable_component_id.value() + ".";
        failures->emplace(prefix + "phase", std::move(phase));
        if (error != nullptr) {
          failures->emplace(prefix + "code", error->code);
          failures->emplace(prefix + "message", error->message);
          for (const auto &[key, value] : error->details)
            failures->emplace(prefix + key, value);
        }
      };
      const auto channel =
          motion ? (candidate.runtime != nullptr
                        ? candidate.runtime->motion_state_channel
                        : nullptr)
                 : (candidate.runtime != nullptr
                        ? candidate.runtime->gripper_state_channel
                        : candidate.gripper_runtime != nullptr
                              ? std::dynamic_pointer_cast<
                                    RetainedObservedStateChannel>(
                                    candidate.gripper_runtime->state_channel())
                              : nullptr);
      if (!channel) {
        refuse("observation_channel_missing");
        return;
      }
      const auto physical = channel->binding();
      if (!physical.value().control_path.has_value()) {
        refuse("physical_control_path_missing");
        return;
      }
      std::vector<core::RuntimeParticipantBinding> representatives;
      if (candidate.runtime != nullptr) {
        const auto configuration =
            candidate.runtime->ReadCurrentPublishedRouteConfiguration(
                physical.value().control_path->control_domain_id);
        if (!configuration.ok()) {
          refuse("current_route_read_failed", &configuration.error());
          return;
        }
        if (!configuration.value()) {
          refuse("current_route_missing");
          return;
        }
        const auto selected = candidate.runtime->ReadCurrentPhysicalParticipants(
            *configuration.value());
        if (!selected.ok()) {
          refuse("physical_participants_read_failed", &selected.error());
          return;
        }
        representatives = selected.value();
      } else {
        const auto route = candidate.active_addressed_kernel()
                               ->current_runtime_route_configuration(
                                   physical.value().control_path->control_domain_id);
        if (!route.ok() || !route.value().has_value() ||
            !route.value()->operation_admission_open ||
            route.value()->active_configuration
                    .active_control_configuration_generation !=
                physical.value()
                    .control_path->active_control_configuration_generation ||
            route.value()->active_configuration
                    .active_control_configuration_id !=
                physical.value().control_path->active_control_configuration_id) {
          refuse("current_route_unqualified", route.ok() ? nullptr : &route.error());
          return;
        }
        representatives.push_back(physical);
      }
      const auto resolved =
          composition.target_resolver->ResolveObservationTarget(
              program_binding_id, target_role_id, capability_id, physical,
              representatives);
      if (!resolved.ok()) {
        refuse("sealed_target_resolution_failed", &resolved.error());
        return;
      }
      if (match.has_value()) {
        ambiguous = true;
        if (failures != nullptr)
          failures->emplace("reason", "observation_owner_ambiguous");
        return;
      }
      match.emplace(ResolvedObservationOwner{&candidate, channel, physical,
                                             resolved.value()});
    };
    inspect(composition);
    for (const auto &participant : composition.participant_compositions)
      inspect(*participant);
    return ambiguous ? std::nullopt : match;
  }

[[nodiscard]] Composition *
  ExecutionCommandSurface::FindBindingComposition(const core::RuntimeParticipantBindingDTO &binding,
                         Composition &composition) const {
    Composition *match = nullptr;
    bool ambiguous = false;
    const auto inspect = [&](Composition &candidate) {
      const auto same_target = [&](const auto &owned) {
        return owned.value().controllable_component_id ==
                   binding.controllable_component_id &&
               owned.value().command_target_id == binding.command_target_id;
      };
      const bool owns_target =
          (candidate.direct_control_motion_binding.has_value() &&
           same_target(*candidate.direct_control_motion_binding)) ||
          (candidate.direct_control_gripper_binding.has_value() &&
           same_target(*candidate.direct_control_gripper_binding));
      if (!owns_target)
        return;
      if (match != nullptr) {
        ambiguous = true;
        return;
      }
      match = &candidate;
    };
    inspect(composition);
    for (const auto &participant : composition.participant_compositions)
      inspect(*participant);
    return ambiguous ? nullptr : match;
  }

[[nodiscard]] core::Result<core::GripperCommandStatusDTO>
  ExecutionCommandSurface::ReadCurrentGripperStatus(
      Composition &component,
      const core::RuntimeParticipantBinding &observation_binding) const {
    if (component.runtime != nullptr)
      return component.runtime->ReadGripperStatus(observation_binding);
    if (component.gripper_runtime == nullptr ||
        component.gripper_runtime->backend() == nullptr ||
        observation_binding.value().controllable_component_id !=
            component.controllable_component_id ||
        observation_binding.value().command_target_id !=
            component.gripper_runtime->binding().value().command_target_id) {
      return core::Result<core::GripperCommandStatusDTO>::Err(core::ErrorDTO{
          "gripper_feedback_unavailable",
          "target has no exact retained gripper feedback owner", {}});
    }
    try {
      return core::Result<core::GripperCommandStatusDTO>::Ok(
          component.gripper_runtime->backend()->get_gripper_status(
              observation_binding.value().command_target_id.value()));
    } catch (const std::exception &exception) {
      return core::Result<core::GripperCommandStatusDTO>::Err(core::ErrorDTO{
          "gripper_feedback_unavailable", exception.what(), {}});
    }
  }

[[nodiscard]] CanonicalValue
  ExecutionCommandSurface::FrameTransformResponse(const TargetObservationQuery &query,
                         Composition &composition) const {
    const auto &transform = *query.frame_transform;
    for (const auto *reference :
         {&transform.source_frame, &transform.target_frame}) {
      const auto verified = VerifyReferenceTarget(
          *composition.target_resolver, query.program_binding_id, *reference);
      if (!verified.ok())
        return CanonicalValue(CanonicalValue::Object{
            {"api_version", CanonicalValue(std::string(kResponseApiVersion))},
            {"error", ErrorValue(verified.error())},
            {"ok", CanonicalValue(false)},
        });
    }
    if (composition.scene_frame_resolver == nullptr)
      return ErrorResponse("execution_frame_transform_unavailable",
                           "cell has no installed frame resolver");
    std::optional<core::CollisionWorldLeaseInventory> inventory;
    if (composition.planner_scene_projection != nullptr) {
      inventory = composition.planner_scene_projection
                      ->current_scene_snapshot_lease_inventory();
    } else if (composition.runtime != nullptr &&
               composition.runtime->collision_publisher != nullptr) {
      inventory = composition.runtime->collision_publisher
                      ->current_scene_snapshot_lease_inventory();
    }
    if (!inventory.has_value() || !inventory->logical_scene)
      return ErrorResponse("execution_frame_transform_unavailable",
                           "cell has no current retained scene projection");
    const auto now_ns = composition.cell_runtime != nullptr
                            ? composition.cell_runtime->clock()->now()
                        : composition.runtime != nullptr
                            ? composition.runtime->clock->now()
                            : std::int64_t{-1};
    const auto resolved = composition.scene_frame_resolver->ResolveFrameTransform(
        RetainedSceneFrameTransformQuery{transform.pose, transform.source_frame,
                                         transform.target_frame,
                                         inventory->projection_binding},
        inventory->logical_scene, now_ns);
    if (!resolved.ok())
      return CanonicalValue(CanonicalValue::Object{
          {"api_version", CanonicalValue(std::string(kResponseApiVersion))},
          {"error", ErrorValue(resolved.error())},
          {"ok", CanonicalValue(false)},
      });
    CanonicalValue::Array evidence;
    for (const auto &digest : resolved.value().resolution_evidence_digests)
      evidence.emplace_back(digest.value());
    return CanonicalValue(CanonicalValue::Object{
        {"api_version", CanonicalValue(std::string(kResponseApiVersion))},
        {"command", CanonicalValue("query_target")},
        {"ok", CanonicalValue(true)},
        {"target_observation",
         CanonicalValue(CanonicalValue::Object{
             {"evaluated_at_ns",
              content_identity::nonnegative_int64_value(
                  resolved.value().evaluated_at_ns)},
             {"kind", CanonicalValue("frame_transform")},
             {"owner_epoch", CanonicalValue(owner_epoch_.value())},
             {"pose", transform.pose_value},
             {"program_binding_id",
              CanonicalValue(query.program_binding_id.value())},
             {"resolution_evidence_digests", CanonicalValue(std::move(evidence))},
             {"scene_revision",
              SceneRevisionValue(resolved.value()
                                     .projection_binding.projection_revision
                                     .scene_revision)},
             {"schema_version",
              CanonicalValue("manyforge.target_observation.v1")},
             {"source_frame", transform.source_frame_value},
             {"target_frame", transform.target_frame_value},
             {"value", PoseValue(resolved.value().pose_in_target_frame)},
         })},
    });
  }

CanonicalValue ExecutionCommandSurface::IoSignalQueryResponse(
    const TargetObservationQuery &query, Composition &composition) const {
  const auto observed = ReadBoundIoSignal(query, composition);
  if (!observed.ok())
    return ErrorResponse(observed.error().code, observed.error().message,
                         observed.error().details);
  return IoSignalResponse(query, observed.value(), "query_target");
}

core::Result<core::IoSignalObservationDTO> ExecutionCommandSurface::ReadBoundIoSignal(
    const TargetObservationQuery &query, Composition &composition) const {
  using Reply = core::Result<core::IoSignalObservationDTO>;
  if (query.owner_epoch != owner_epoch_)
    return Reply::Err({"execution_target_owner_stale", "target query names a replaced owner", {}});
  if (!composition.io_signal_service)
    return Reply::Err({"io_signal_service_unavailable",
                      "current activation has no installed I/O service", {}});
  const deployment::ProgramBinding *binding = nullptr;
  const auto find = [&](const Composition &view) {
    for (const auto &candidate :
         {view.program_binding, view.direct_control_binding,
          view.direct_control_gripper_program_binding}) {
      if (candidate &&
          candidate->program_binding_id() == query.program_binding_id)
        binding = candidate.get();
    }
    for (const auto &candidate : view.direct_control_io_program_bindings)
      if (candidate &&
          candidate->program_binding_id() == query.program_binding_id)
        binding = candidate.get();
  };
  find(composition);
  for (const auto &peer : composition.participant_compositions) {
    Require(peer != nullptr, "I/O query has a missing component view");
    find(*peer);
  }
  if (!binding || binding->value().retained_bundle_id !=
                      composition.bundle->retained_bundle_id())
    return Reply::Err({"io_signal_unbound", "I/O query has no exact current Program Binding", {}});
  const auto &targets = binding->value().target_role_bindings;
  const auto target =
      std::find_if(targets.begin(), targets.end(), [&](const auto &row) {
        return row.target_role_id == query.target_role_id;
      });
  if (target == targets.end())
    return Reply::Err({"io_signal_unbound", "I/O query target role is not bound", {}});
  const auto &signals = target->signal_role_bindings;
  const auto signal =
      std::find_if(signals.begin(), signals.end(), [&](const auto &row) {
        return row.resource_role_id == *query.io_signal_role;
      });
  if (signal == signals.end())
    return Reply::Err({"io_signal_unbound", "I/O query resource role is not a signal", {}});
  if (signal->direction != core::IoSignalDirection::kInput)
    return Reply::Err({"io_signal_direction_mismatch",
                      "I/O query requires a separately bound input signal", {}});
  const auto observed = composition.io_signal_service->ReadSignal(
      {target->controllable_component_id, signal->signal_name});
  if (!observed.ok())
    return Reply::Err(observed.error());
  if (observed.value().cursor.owner_epoch != owner_epoch_ ||
      observed.value().cursor.catalog_generation !=
          composition.publication_generation.value())
    return Reply::Err({"io_signal_catalog_stale", "I/O service differs from current activation", {}});
  const auto digest =
      core::ComputeIoSignalDescriptorDigest(observed.value().descriptor);
  if (!digest.ok())
    return Reply::Err(digest.error());
  if (digest.value() != signal->signal_descriptor_digest)
    return Reply::Err({"io_signal_catalog_stale", "I/O descriptor differs from sealed binding", {}});
  return observed;
}

CanonicalValue ExecutionCommandSurface::MockIoInputResponse(
    const MockIoInputSetRequest &request, Composition &composition) const {
  const auto before = ReadBoundIoSignal(request.query, composition);
  if (!before.ok())
    return ErrorResponse(before.error().code, before.error().message,
                         before.error().details);
  const auto changed =
      SetCompositionMockInput(composition, before.value().descriptor.signal_id,
                              request.expected_source_epoch, request.value);
  if (!changed.ok())
    return ErrorResponse(changed.error().code, changed.error().message,
                         changed.error().details);
  const auto &result = changed.value();
  auto response =
      IoSignalResponse(request.query, result.observation, "set_mock_io_input")
          .as_object();
  response.emplace_back("model_value", CanonicalValue(result.model_value));
  response.emplace_back("published_cursor",
                        result.published_cursor
                            ? IoSourceCursorValue(*result.published_cursor)
                            : CanonicalValue(nullptr));
  if (result.publication_error)
    response.emplace_back("publication_error",
                          ErrorValue(*result.publication_error));
  return CanonicalValue(std::move(response));
}

CanonicalValue ExecutionCommandSurface::IoSignalResponse(
    const TargetObservationQuery &query,
    const core::IoSignalObservationDTO &observation,
    std::string_view command) const {
  return CanonicalValue(CanonicalValue::Object{
      {"api_version", CanonicalValue(std::string(kResponseApiVersion))},
      {"command", CanonicalValue(std::string(command))},
      {"ok", CanonicalValue(true)},
      {"target_observation",
       CanonicalValue(CanonicalValue::Object{
           {"schema_version",
            CanonicalValue("manyforge.target_observation.v1")},
           {"owner_epoch", CanonicalValue(owner_epoch_.value())},
           {"program_binding_id",
            CanonicalValue(query.program_binding_id.value())},
           {"target_role_id", CanonicalValue(query.target_role_id.value())},
           {"resource_role_id", CanonicalValue(query.io_signal_role->value())},
           {"kind", CanonicalValue("io_signal")},
           {"evaluated_at_ns", content_identity::nonnegative_int64_value(
                                   observation.evaluated_at_ns)},
           {"value", IoSignalObservationValue(observation)}})}});
}

CanonicalValue ExecutionCommandSurface::ProgramBlackboardResponse(
    const core::IoProgramBlackboardPublicationDTO &publication,
    core::ResourceAddressedExecutionKernel &owner) const {
  if (composition_access_.prepare_program_blackboard_context) {
    const auto prepared = composition_access_.prepare_program_blackboard_context(publication.scope);
    if (!prepared.ok())
      return ErrorResponse(prepared.error().code, prepared.error().message,
                           prepared.error().details);
  }
  // Keep the actual publication/service selection pinned through every frame,
  // including cheap steady-state writes. The retained Kernel context alone
  // cannot observe an in-progress host composition replacement.
  std::shared_lock composition_lock(composition_access_.mutex);
  const auto context = owner.applied_program_blackboard_context();
  const auto *current = composition_access_.product();
  if (context && (!current || !current->bundle || !current->program_binding ||
      !current->io_signal_service ||
      current->publication_generation != context->active_publication_generation ||
      current->bundle->retained_bundle_id() != context->retained_bundle_id ||
      current->bundle->value().cell_graph.graph_revision != context->graph_revision ||
      current->program_binding->program_binding_id() != context->program_binding_id))
    return ErrorResponse("stale_generation", "The actual Applied Program source changed",
                         {{"reason", "program_bb_scope_stale"}});
  const auto admitted = owner.PublishProgramBlackboard(publication);
  if (!admitted.ok())
    return ErrorResponse(admitted.error().code, admitted.error().message,
                         admitted.error().details);
  if (current && current->io_signal_runtime)
    current->io_signal_runtime->RequestRuleEvidenceProcessing();
  const auto &ack = admitted.value();
  Require(ack.cycle_generation > 0U &&
              ack.cycle_generation <= 9007199254740991ULL &&
              ack.active_publication_generation.value() > 0U &&
              ack.active_publication_generation.value() <= 9007199254740991ULL,
          "Program BB ACK occurrence exceeds its existing journal bound");
  const bool accepted =
      ack.status == core::IoProgramBlackboardAcknowledgementStatus::kAccepted;
  Require((accepted ||
           ack.status ==
               core::IoProgramBlackboardAcknowledgementStatus::kSuperseded) &&
              accepted == ack.accepted_sequence.has_value() &&
              (!ack.accepted_sequence || *ack.accepted_sequence > 0U),
          "Program BB ACK status and admitted sequence are inconsistent");
  return CanonicalValue(CanonicalValue::Object{
      {"api_version", CanonicalValue(std::string(kResponseApiVersion))},
      {"command", CanonicalValue("publish_program_bbkeys")},
      {"ok", CanonicalValue(true)},
      {"status", CanonicalValue(accepted ? "accepted" : "superseded")},
      {"owner_epoch", CanonicalValue(ack.owner_epoch.value())},
      {"program_binding_id", CanonicalValue(ack.program_binding_id.value())},
      {"active_publication_generation",
       CanonicalValue(
           static_cast<double>(ack.active_publication_generation.value()))},
      {"cycle_generation",
       CanonicalValue(static_cast<double>(ack.cycle_generation))},
      {"program_run_id", ack.program_run_id
                             ? CanonicalValue(ack.program_run_id->value())
                             : CanonicalValue(nullptr)},
      {"source_epoch", CanonicalValue(ack.source_epoch.value())},
      {"accepted_sequence",
       ack.accepted_sequence
           ? CanonicalValue(std::to_string(*ack.accepted_sequence))
           : CanonicalValue(nullptr)}});
}

CanonicalValue ExecutionCommandSurface::MockRunPauseResponse(
    const core::ProgramRunMockSimulationPauseRequestDTO& request,
    core::ResourceAddressedExecutionKernel& owner) const {
  const auto accepted = owner.SetProgramRunMockSimulationPaused(request);
  if (!accepted.ok()) return ErrorResponse(accepted.error().code, accepted.error().message, accepted.error().details);
  const auto& scope = request.scope;
  CanonicalValue::Array held_nodes;
  held_nodes.reserve(request.held_node_instance_ids.size());
  for (const auto& id : request.held_node_instance_ids)
    held_nodes.emplace_back(id.value());
  return CanonicalValue(CanonicalValue::Object{
      {"api_version", CanonicalValue(std::string(kResponseApiVersion))},
      {"command", CanonicalValue("set_mock_io_run_paused")}, {"ok", CanonicalValue(true)},
      {"owner_epoch", CanonicalValue(scope.request_owner_epoch.value())},
      {"program_binding_id", CanonicalValue(scope.program_binding_id.value())},
      {"program_runner_owner_epoch", CanonicalValue(scope.program_runner_owner_epoch.value())},
      {"claim_generation", CanonicalValue(static_cast<double>(scope.claim_generation.value()))},
      {"program_run_id", CanonicalValue(scope.program_run_id.value())},
      {"run_revision", CanonicalValue(std::to_string(request.run_revision))},
      {"paused", CanonicalValue(request.paused)},
      {"held_node_instance_ids", CanonicalValue(std::move(held_nodes))}});
}

CanonicalValue ExecutionCommandSurface::MockNodeSimulationResponse(
    const core::ProgramNodeMockSimulationScopeDTO& requested_scope,
    const std::vector<core::ProgramMockSimulationSignalDTO>* requested_signals,
    ExecutionProduct& product, core::ResourceAddressedExecutionKernel& owner) const {
  const core::ProgramNodeMockSimulationRequestDTO request{
      requested_scope, requested_signals ? *requested_signals : std::vector<core::ProgramMockSimulationSignalDTO>{}};
  const auto mismatch = [] {
    return ErrorResponse("invalid_request", "Mock simulation does not match its exact sealed Program node",
                         {{"reason", "mock_simulation_node_mismatch"}});
  };
  // This SAME shared product lease stays held through the Kernel's final
  // current capture/claim join. No public packet can supply this Program root.
  if (!product.bundle || !product.program || !product.program_binding ||
      product.program_binding->program_binding_id() != request.scope.program_binding_id)
    return mismatch();
  const auto& bound = product.program_binding->value();
  if (bound.program_id != product.program->value().program_id ||
      bound.program_revision != product.program->value().program_revision ||
      bound.retained_bundle_id != product.bundle->retained_bundle_id() ||
      bound.graph_revision != product.bundle->value().cell_graph.graph_revision)
    return mismatch();
  const auto& root = product.program->normalized_program_root();
  const auto* tree = root.find_object_member("tree");
  if (!tree) return mismatch();
  const CanonicalValue* selected = nullptr;
  std::vector<const CanonicalValue*> pending{tree};
  std::size_t visited = 0U;
  while (!pending.empty()) {
    const auto* node = pending.back();
    pending.pop_back();
    if (++visited > core::kMaxProgramTreeNodes || !node->is_object()) return mismatch();
    if (Text(*node, "instance_id") == request.scope.node.node_instance_id.value()) {
      if (selected) return mismatch();
      selected = node;
    }
    for (const auto& child : Member(*node, "children").as_array()) pending.push_back(&child);
  }
  if (!selected) return mismatch();
  const auto kind = Text(*selected, "node_id");
  const bool motion = kind == "motion.move_until_signal" || kind == "motion.probe_search";
  if (!motion && kind != "io.get_input" && kind != "io.check_input" && kind != "io.wait_for_input")
    return mismatch();
  const auto& parameters = Member(*selected, "parameters");
  const auto* rows = parameters.find_object_member("mock_simulation");
  if (!rows || !rows->is_array() || rows->as_array().empty()) return mismatch();
  // Normalize only authoring omission/order, then use the SAME closed row
  // decoder as the packet. Never duplicate the condition compiler/walker.
  auto normalized = rows->as_array();
  for (auto& row : normalized) {
    if (!row.find_object_member("change")) {
      auto fields = row.as_object();
      fields.emplace_back("change", CanonicalValue(nullptr));
      row = CanonicalValue(std::move(fields));
    }
  }
  std::sort(normalized.begin(), normalized.end(), [](const auto& left, const auto& right) {
    const auto& a = Member(left, "signal"); const auto& b = Member(right, "signal");
    return std::pair{Text(a, "target_role"), Text(a, "resource_role")} <
           std::pair{Text(b, "target_role"), Text(b, "resource_role")};
  });
  const auto actual = DecodeProgramMockSimulationSignals(CanonicalValue(std::move(normalized)));
  if (requested_signals && actual != request.signals) return mismatch();
  if (!motion) {
    for (const auto& row : actual)
      if (row.target_role_id.value() != Text(parameters, "target_role") ||
          row.resource_role_id.value() != Text(parameters, "resource_role")) return mismatch();
  }
  if (!requested_signals) {
    const auto ended = owner.EndProgramNodeMockSimulation(request.scope);
    if (!ended.ok()) return ErrorResponse(ended.error().code, ended.error().message, ended.error().details);
    const auto& scope = request.scope;
    return CanonicalValue(CanonicalValue::Object{
        {"api_version", CanonicalValue(std::string(kResponseApiVersion))},
        {"command", CanonicalValue("end_mock_io_node_simulation")}, {"ok", CanonicalValue(true)},
        {"owner_epoch", CanonicalValue(scope.request_owner_epoch.value())},
        {"program_binding_id", CanonicalValue(scope.program_binding_id.value())},
        {"program_runner_owner_epoch", CanonicalValue(scope.program_runner_owner_epoch.value())},
        {"claim_generation", CanonicalValue(static_cast<double>(scope.claim_generation.value()))},
        {"program_run_id", CanonicalValue(scope.program_run_id.value())},
        {"node_instance_id", CanonicalValue(scope.node.node_instance_id.value())},
        {"node_attempt_id", CanonicalValue(scope.node.node_attempt_id)},
        {"node_start_index", CanonicalValue(std::to_string(scope.node_start_index))}});
  }
  const auto started = owner.StartProgramNodeMockSimulation(request);
  if (!started.ok()) return ErrorResponse(started.error().code, started.error().message, started.error().details);
  const auto& receipt = started.value();
  Require(receipt.scope == request.scope && receipt.started_at_ns >= 0 &&
              receipt.signals.size() == request.signals.size(),
          "Mock simulation acknowledgement differs from its captured request");
  CanonicalValue::Array signals;
  for (std::size_t index = 0; index < receipt.signals.size(); ++index) {
    const auto& row = receipt.signals[index];
    Require(row.target_role_id == request.signals[index].target_role_id &&
                row.resource_role_id == request.signals[index].resource_role_id &&
                (row.disposition == core::ProgramMockSimulationSignalDisposition::kApplied ||
                 row.disposition == core::ProgramMockSimulationSignalDisposition::kInactive),
            "Mock simulation acknowledgement has an invalid signal disposition");
    signals.emplace_back(CanonicalValue::Object{
        {"signal", CanonicalValue(CanonicalValue::Object{
            {"target_role", CanonicalValue(row.target_role_id.value())},
            {"resource_role", CanonicalValue(row.resource_role_id.value())}})},
        {"status", CanonicalValue(row.disposition == core::ProgramMockSimulationSignalDisposition::kApplied
                                      ? "applied" : "inactive")}});
  }
  const auto& scope = receipt.scope;
  return CanonicalValue(CanonicalValue::Object{
      {"api_version", CanonicalValue(std::string(kResponseApiVersion))},
      {"command", CanonicalValue("start_mock_io_node_simulation")},
      {"ok", CanonicalValue(true)}, {"status", CanonicalValue("accepted")},
      {"owner_epoch", CanonicalValue(scope.request_owner_epoch.value())},
      {"program_binding_id", CanonicalValue(scope.program_binding_id.value())},
      {"program_runner_owner_epoch", CanonicalValue(scope.program_runner_owner_epoch.value())},
      {"claim_generation", CanonicalValue(static_cast<double>(scope.claim_generation.value()))},
      {"program_run_id", CanonicalValue(scope.program_run_id.value())},
      {"node_instance_id", CanonicalValue(scope.node.node_instance_id.value())},
      {"node_attempt_id", CanonicalValue(scope.node.node_attempt_id)},
      {"node_start_index", CanonicalValue(std::to_string(scope.node_start_index))},
      {"started_at_ns", CanonicalValue(std::to_string(receipt.started_at_ns))},
      {"signals", CanonicalValue(std::move(signals))}});
}

CanonicalValue ExecutionCommandSurface::IoWatchResponse(
    const Command &command,
    core::ResourceAddressedExecutionKernel &owner) const {
  if (command.kind == CommandKind::kRegisterIoWatch) {
    if (command.io_watch_condition_unsupported)
      return ErrorResponse("io_watch_condition_unsupported",
                           "only boolean level watches are installed");
    const auto registered =
        owner.RegisterProgramIoWatch(*command.io_watch_request);
    if (!registered.ok())
      return ErrorResponse(registered.error().code, registered.error().message,
                           registered.error().details);
    const auto observed = owner.ReadProgramIoWatch(registered.value());
    if (!observed.ok())
      return ErrorResponse(observed.error().code, observed.error().message,
                           observed.error().details);
    return CanonicalValue(CanonicalValue::Object{
        {"api_version", CanonicalValue(std::string(kResponseApiVersion))},
        {"command", CanonicalValue("register_io_watch")},
        {"ok", CanonicalValue(true)},
        {"registration", IoWatchRegistrationValue(registered.value())},
        {"result", IoWatchResultValue(observed.value())}});
  }
  if (command.kind == CommandKind::kReadIoWatch) {
    const auto observed =
        owner.ReadProgramIoWatch(*command.io_watch_registration);
    if (!observed.ok())
      return ErrorResponse(observed.error().code, observed.error().message,
                           observed.error().details);
    return CanonicalValue(CanonicalValue::Object{
        {"api_version", CanonicalValue(std::string(kResponseApiVersion))},
        {"command", CanonicalValue("read_io_watch")},
        {"ok", CanonicalValue(true)},
        {"result", IoWatchResultValue(observed.value())}});
  }
  Require(command.kind == CommandKind::kReleaseIoWatch,
          "I/O watch dispatch is inconsistent");
  const auto released =
      command.io_watch_scope_release
          ? owner.ReleaseProgramIoWatchScope(*command.io_watch_scope_release)
          : owner.ReleaseProgramIoWatch(*command.io_watch_registration);
  if (!released.ok())
    return ErrorResponse(released.error().code, released.error().message,
                         released.error().details);
  return CanonicalValue(CanonicalValue::Object{
      {"api_version", CanonicalValue(std::string(kResponseApiVersion))},
      {"command", CanonicalValue("release_io_watch")},
      {"ok", CanonicalValue(true)}});
}
[[nodiscard]] CanonicalValue
  ExecutionCommandSurface::QueryTargetResponse(const TargetObservationQuery &query,
                      Composition &composition) const {
    if (query.owner_epoch != owner_epoch_)
      return ErrorResponse("execution_target_owner_stale",
                           "target query names a replaced owner");
    if (query.io_signal_role.has_value())
      return IoSignalQueryResponse(query, composition);
    if (query.frame_transform.has_value())
      return FrameTransformResponse(query, composition);
    const bool frame_query = query.frame_role_id.has_value();
    const auto capability_id = core::CapabilityId(
        frame_query ? "model.kinematics.fk" : "tool.gripper.feedback");
    std::map<std::string, std::string> failures;
    const auto owner =
        ResolveObservationOwner(query.program_binding_id, query.target_role_id,
                                capability_id, frame_query, composition, &failures);
    if (!owner.has_value())
      return ErrorResponse("execution_target_observation_unavailable",
                           "target has no unique qualified observation owner",
                           std::move(failures));
    auto &selected = *owner->composition;
    const auto &channel = owner->channel;
    const auto &observed_binding = owner->physical;
    const auto &binding = observed_binding.value();
    const auto &resolved = owner->resolved;
    const auto &target = resolved.target.target_role();
    const auto* gripper_backend = selected.runtime ? selected.runtime->gripper.get()
        : selected.gripper_runtime ? selected.gripper_runtime->backend() : nullptr;
    if (gripper_backend && gripper_backend->gripper_actuation_kind(binding.command_target_id.value()) ==
            core::GripperActuationKind::kPneumatic)
      return ErrorResponse("gripper_state_unavailable",
          "Pneumatic finger coordinates are display-only, not qualified gripper state");
    const auto observed = channel->current_state(
        selected.cell_runtime != nullptr ? selected.cell_runtime->clock()->now()
                                         : selected.runtime->clock->now());
    if (!observed.ok())
      return ErrorResponse(observed.error().code, observed.error().message);
    const auto &state = observed.value();
    std::optional<core::ActiveControlConfigurationDTO> current_configuration;
    if (selected.runtime != nullptr) {
      const auto read = selected.runtime->ReadCurrentPublishedRouteConfiguration(
          state.control_domain_id);
      if (read.ok())
        current_configuration = read.value();
    } else {
      const auto read = selected.active_addressed_kernel()
                            ->current_runtime_route_configuration(
                                state.control_domain_id);
      if (read.ok() && read.value().has_value() &&
          read.value()->operation_admission_open)
        current_configuration = read.value()->active_configuration;
    }
    if (!current_configuration.has_value() ||
        !target_observation_source_is_current(resolved.observation_binding,
                                              state,
                                              *current_configuration))
      return ErrorResponse("execution_target_observation_unavailable",
                           "selected read source is no longer reconciled");
    Require(state.complete &&
                state.controllable_component_id ==
                    binding.controllable_component_id &&
                state.command_target_id == binding.command_target_id &&
                state.resources.size() == binding.resources.size() &&
                state.joint_names.size() == binding.resources.size() &&
                state.positions.size() == binding.resources.size(),
            "target query observation crosses its retained binding");
    std::vector<double> positions;
    positions.reserve(binding.resources.size());
    for (std::size_t index = 0; index < binding.resources.size(); ++index) {
      const auto &resource = binding.resources[index];
      Require(state.resources[index] == resource.resource_id &&
                  state.joint_names[index] == resource.compiled_resource_name &&
                  core::coordinate_kind(state.positions[index]) ==
                      resource.coordinate_kind,
              "target query observation has mismatched coordinates");
      const auto *angular =
          std::get_if<core::AngularPositionDTO>(&state.positions[index]);
      const auto *linear =
          std::get_if<core::LinearPositionDTO>(&state.positions[index]);
      Require(angular != nullptr || linear != nullptr,
              "target query observation has unsupported coordinate units");
      const double value = angular ? angular->value_rad : linear->value_m;
      Require(std::isfinite(value), "target query observation is not finite");
      positions.push_back(value);
    }
    CanonicalValue value;
    if (frame_query) {
      if (selected.runtime == nullptr ||
          selected.runtime->kinematic_model == nullptr)
        return ErrorResponse(
            "execution_target_frame_unavailable",
            "target has no native kinematics owner for frame observation");
      const auto frame =
          std::find_if(target.frame_role_bindings.begin(),
                       target.frame_role_bindings.end(), [&](const auto &row) {
                         return row.frame_role_id == *query.frame_role_id;
                       });
      if (frame == target.frame_role_bindings.end())
        return ErrorResponse(
            "execution_target_frame_unavailable",
            "frame role is not declared by the target binding");
      if (!composition.target_resolver->FrameObservationCovered(
              *frame, observed_binding))
        return ErrorResponse(
            "execution_target_frame_state_unavailable",
            "frame depends on coordinates outside the qualified observation");
      std::string base_frame;
      if (query.relative_to_frame_role_id.has_value()) {
        const auto base = std::find_if(
            target.frame_role_bindings.begin(),
            target.frame_role_bindings.end(), [&](const auto &row) {
              return row.frame_role_id == *query.relative_to_frame_role_id;
            });
        if (base == target.frame_role_bindings.end())
          return ErrorResponse(
              "execution_target_frame_unavailable",
              "relative frame role is not declared by the target binding");
        if (!composition.target_resolver->FrameObservationCovered(
                *base, observed_binding))
          return ErrorResponse("execution_target_frame_state_unavailable",
                               "relative frame depends on coordinates outside "
                               "the qualified observation");
        base_frame = base->compiled_frame_name;
      }
      // Empty base retains the existing logical-cell-frame query semantics.
      const auto pose =
          selected.runtime->kinematic_model->forward_kinematics(core::FkRequestDTO{
              core::RobotStateDTO{state.joint_names, std::move(positions), {}},
              frame->compiled_frame_name, std::move(base_frame)});
      if (!pose.ok())
        return ErrorResponse(pose.error().code, pose.error().message);
      value = PoseValue(pose.value());
    } else {
      if ((selected.runtime == nullptr || !selected.runtime->gripper) &&
          selected.gripper_runtime == nullptr)
        return ErrorResponse("gripper_feedback_unavailable",
                             "target has no qualified gripper feedback");
      if (binding.resources.size() != 1U)
        return ErrorResponse("gripper_feedback_unavailable",
                             "target has no qualified gripper feedback");
      const auto current = ReadCurrentGripperStatus(
          selected, resolved.observation_binding);
      if (!current.ok())
        return ErrorResponse(current.error().code, current.error().message);
      const auto &status = current.value();
      const auto &source = *resolved.observation_binding.value().control_path;
      value = CanonicalValue(CanonicalValue::Object{
          {"source_id", CanonicalValue(source.control_binding_id.value())},
          {"source_revision",
           CanonicalValue(source.control_binding_revision.value())},
          {"source_epoch",
           CanonicalValue(source.provider_runtime_epoch.value())},
          {"active_control_configuration_id",
           CanonicalValue(source.active_control_configuration_id.value())},
          {"active_control_configuration_generation",
           CanonicalValue(static_cast<double>(
               source.active_control_configuration_generation.value()))},
          {"command_id",
           CanonicalValue(static_cast<double>(status.command_id))},
          {"generation",
           CanonicalValue(static_cast<double>(status.generation))},
          // No command echo or unqualified evidence label proves a grasp.
          {"grasp_confirmed", CanonicalValue(nullptr)},
          {"observed_position",
           CoordinatePositionValue(state.positions.front())},
          {"revision", CanonicalValue(static_cast<double>(status.revision))},
          {"state",
           CanonicalValue(core::gripper_command_state_name(status.state))},
      });
    }
    CanonicalValue::Object result{
        {"kind", CanonicalValue(frame_query ? "frame_pose" : "gripper_status")},
        {"owner_epoch", CanonicalValue(owner_epoch_.value())},
        {"program_binding_id",
         CanonicalValue(query.program_binding_id.value())},
        {"schema_version", CanonicalValue("manyforge.target_observation.v1")},
        {"source_epoch", CanonicalValue(state.source_epoch.value())},
        {"source_sequence",
         CanonicalValue(static_cast<double>(state.sequence))},
        {"target_role_id", CanonicalValue(query.target_role_id.value())},
        {"value", std::move(value)},
    };
    if (frame_query)
      result.emplace_back("frame_role_id",
                          CanonicalValue(query.frame_role_id->value()));
    if (query.relative_to_frame_role_id.has_value())
      result.emplace_back(
          "relative_to_frame_role_id",
          CanonicalValue(query.relative_to_frame_role_id->value()));
    return CanonicalValue(CanonicalValue::Object{
        {"api_version", CanonicalValue(std::string(kResponseApiVersion))},
        {"command", CanonicalValue("query_target")},
        {"ok", CanonicalValue(true)},
        {"target_observation", CanonicalValue(std::move(result))},
    });
  }

[[nodiscard]] CanonicalValue
  ExecutionCommandSurface::QueryOperationResponse(const ProgramOperationQuery &query,
                         Composition &composition) const {
    if (query.owner_epoch != owner_epoch_)
      return ErrorResponse("stale_request_owner_epoch",
                           "operation query names another native owner");
    const auto target = composition.target_resolver->ResolveOperationTarget(
        query.program_binding_id, query.target_role_id);
    if (!target.ok())
      return ErrorResponse(target.error().code, target.error().message);
    // This is one bounded immutable owner capture, not a new operation index.
    const auto operations =
        composition.active_addressed_kernel()->get_resident_operations(
            {query.program_run_id});
    const core::OperationStateDTO *selected = nullptr;
    for (const auto &operation : operations) {
      if (operation.program_binding_id != query.program_binding_id)
        continue;
      const auto matches = std::count_if(
          operation.participants.begin(), operation.participants.end(),
          [&](const auto &participant) {
            return participant.controllable_component_id ==
                       target.value().controllable_component_id &&
                   participant.command_target_id ==
                       target.value().command_target_id;
          });
      if (matches == 0)
        continue;
      if (selected || operation.participants.size() != 1U)
        return ErrorResponse(
            "execution_operation_context_ambiguous",
            "target does not select one exact operation scope");
      selected = &operation;
    }
    if (!selected)
      return ErrorResponse(
          "execution_operation_context_unavailable",
          "target has no resident operation in the exact Program run");
    CanonicalValue scale_value;
    if (selected->runtime_scale_generation.has_value()) {
      const auto scale =
          composition.active_addressed_kernel()->get_runtime_scale(
              selected->operation_id);
      if (!scale.ok())
        return ErrorResponse(scale.error().code, scale.error().message);
      if (scale.value().operation_revision != selected->operation_revision ||
          scale.value().runtime_scale_generation !=
              *selected->runtime_scale_generation)
        return ErrorResponse("execution_operation_observation_changed",
                             "operation changed during scale observation");
      scale_value = CanonicalValue(CanonicalValue::Object{
          {"generation", CanonicalValue(static_cast<double>(
                             scale.value().runtime_scale_generation.value()))},
          {"state",
           CanonicalValue(std::string(core::to_string(scale.value().state)))},
          {"requested_scale", CanonicalValue(scale.value().requested_scale)},
          {"effective_scale", CanonicalValue(scale.value().effective_scale)},
      });
    }
    return CanonicalValue(CanonicalValue::Object{
        {"api_version", CanonicalValue(std::string(kResponseApiVersion))},
        {"command", CanonicalValue("query_operation")},
        {"ok", CanonicalValue(true)},
        {"operation", OperationStateValue(*selected)},
        {"runtime_scale", std::move(scale_value)},
        {"owner_epoch", CanonicalValue(owner_epoch_.value())},
        {"target_role_id", CanonicalValue(query.target_role_id.value())},
    });
  }

[[nodiscard]] CanonicalValue ExecutionCommandSurface::DirectControlObservedValue(
      Composition &composition,
      const core::ObservedStateSnapshotDTO &observed_state) const {
    const auto &binding = composition.direct_control_motion_binding->value();
    Require(observed_state.complete &&
                observed_state.resources.size() == binding.resources.size() &&
                observed_state.joint_names.size() == binding.resources.size() &&
                observed_state.positions.size() == binding.resources.size(),
            "direct-control observation crosses its retained target");
    CanonicalValue::Array source_joint_names;
    CanonicalValue::Array observed_positions;
    std::vector<double> observed_positions_rad;
    source_joint_names.reserve(binding.resources.size());
    observed_positions.reserve(binding.resources.size());
    observed_positions_rad.reserve(binding.resources.size());
    for (std::size_t index = 0U; index < binding.resources.size(); ++index) {
      const auto &resource = binding.resources[index];
      const auto *position = std::get_if<core::AngularPositionDTO>(
          &observed_state.positions[index]);
      Require(resource.source_joint_name.has_value() &&
                  resource.coordinate_kind == core::CoordinateKind::kAngular &&
                  observed_state.resources[index] == resource.resource_id &&
                  observed_state.joint_names[index] ==
                      resource.compiled_resource_name &&
                  position != nullptr && std::isfinite(position->value_rad),
              "direct-control observation crosses its source-local binding");
      source_joint_names.emplace_back(*resource.source_joint_name);
      observed_positions.emplace_back(position->value_rad);
      observed_positions_rad.push_back(position->value_rad);
    }
    const auto base_frame =
        std::find_if(binding.frame_bindings.begin(),
                     binding.frame_bindings.end(), [](const auto &frame) {
                       return frame.frame_role_id.value() ==
                              deployment::kDirectControlBaseFrameRoleId;
                     });
    const auto tcp_frame =
        std::find_if(binding.frame_bindings.begin(),
                     binding.frame_bindings.end(), [](const auto &frame) {
                       return frame.frame_role_id.value() ==
                              deployment::kDirectControlTcpFrameRoleId;
                     });
    Require(base_frame != binding.frame_bindings.end() &&
                tcp_frame != binding.frame_bindings.end(),
            "direct-control observation has no exact base/TCP frame binding");
    const auto current_tcp =
        composition.runtime->kinematic_model->forward_kinematics(core::FkRequestDTO{
            core::RobotStateDTO{observed_state.joint_names,
                                std::move(observed_positions_rad),
                                {}},
            tcp_frame->compiled_frame_name, base_frame->compiled_frame_name});
    if (!current_tcp.ok()) {
      Fail("direct-control current TCP pose is unavailable: " +
           current_tcp.error().message);
    }
    return CanonicalValue(CanonicalValue::Object{
        {"complete", CanonicalValue(true)},
        {"current_tcp_pose", PoseValue(current_tcp.value())},
        {"positions_rad", CanonicalValue(std::move(observed_positions))},
        {"sequence",
         CanonicalValue(static_cast<double>(observed_state.sequence))},
        {"source_epoch", CanonicalValue(observed_state.source_epoch.value())},
        {"source_joint_names", CanonicalValue(std::move(source_joint_names))},
    });
  }

[[nodiscard]] CanonicalValue ExecutionCommandSurface::DirectControlStatusValue(
      Composition &composition,
      const core::ObservedStateSnapshotDTO &observed_state) const {
    auto fields = DirectControlTargetValue(composition).as_object();
    fields.emplace_back("observed_state", DirectControlObservedValue(
                                              composition, observed_state));
    return CanonicalValue(std::move(fields));
  }

[[nodiscard]] CanonicalValue
  ExecutionCommandSurface::DirectControlGripperTargetValue(Composition &composition) const {
    const auto &binding = composition.direct_control_gripper_binding->value();
    Require(binding.resources.size() == 1U,
            "direct-control gripper status has no exact scalar resource");
    const auto &resource = binding.resources.front();
    const bool model_backed = resource.source_joint_name.has_value();
    const auto limits = composition.runtime != nullptr
                            ? composition.runtime->gripper_position_limits
                            : composition.gripper_runtime->position_limits();
    Require(binding.command_schema.has_value() &&
                model_backed == limits.has_value(),
            "direct-control gripper status crosses its retained binding");
    Require(!limits.has_value() ||
                (limits->coordinate_kind == resource.coordinate_kind &&
                 std::isfinite(limits->lower_position) &&
                 std::isfinite(limits->upper_position) &&
                 limits->lower_position <= limits->upper_position),
            "direct-control gripper status has inconsistent units or limits");
    const auto coordinate_kind =
        resource.coordinate_kind == core::CoordinateKind::kAngular
            ? "angular_rad"
            : "linear_m";
    const auto &source_coordinate_name = model_backed
                                             ? *resource.source_joint_name
                                             : resource.compiled_resource_name;
    return CanonicalValue(CanonicalValue::Object{
        {"command_schema_id",
         CanonicalValue(binding.command_schema->command_schema_id.value())},
        {"command_schema_revision",
         CanonicalValue(
             binding.command_schema->command_schema_revision.value())},
        {"command_target_id",
         CanonicalValue(binding.command_target_id.value())},
        {"component_role_id",
         CanonicalValue(binding.component_role_id.value())},
        {"controllable_component_id",
         CanonicalValue(binding.controllable_component_id.value())},
        {"operation_capability_id",
         CanonicalValue(binding.capability_id.value())},
        {"program_binding_id",
         CanonicalValue(binding.program_binding_id.value())},
        {"program_id", CanonicalValue(binding.program_id.value())},
        {"program_revision", CanonicalValue(binding.program_revision.value())},
        {"program_run_id",
         CanonicalValue(direct_control_gripper_program_run_id_.value())},
        {"resource",
         CanonicalValue(CanonicalValue::Object{
             {"coordinate_kind", CanonicalValue(coordinate_kind)},
             {"max_position", limits.has_value()
                                  ? CanonicalValue(limits->upper_position)
                                  : CanonicalValue(nullptr)},
             {"min_position", limits.has_value()
                                  ? CanonicalValue(limits->lower_position)
                                  : CanonicalValue(nullptr)},
             {"source_coordinate_name", CanonicalValue(source_coordinate_name)},
             {"source_kind",
              CanonicalValue(model_backed ? "model_role" : "component_local")},
         })},
        {"target_role_id", CanonicalValue(binding.target_role_id.value())},
        {"target_descriptor", composition.direct_control_gripper_descriptor},
    });
  }

[[nodiscard]] CanonicalValue ExecutionCommandSurface::DirectControlGripperObservedValue(
      Composition &composition,
      const core::ObservedStateSnapshotDTO *observed_state) const {
    const auto &binding = composition.direct_control_gripper_binding->value();
    Require(binding.resources.size() == 1U,
            "direct-control gripper observation has no exact scalar resource");
    const auto &resource = binding.resources.front();
    const bool model_backed = resource.source_joint_name.has_value();
    if (observed_state == nullptr) {
      Require(!model_backed, "model-backed gripper observation is unavailable");
      return CanonicalValue(nullptr);
    }
    Require(observed_state->complete &&
                observed_state->resources.size() == 1U &&
                observed_state->joint_names.size() == 1U &&
                observed_state->positions.size() == 1U &&
                observed_state->resources.front() == resource.resource_id &&
                observed_state->joint_names.front() ==
                    resource.compiled_resource_name &&
                core::coordinate_kind(observed_state->positions.front()) ==
                    resource.coordinate_kind,
            "direct-control gripper observation crosses its retained target");
    const auto &source_coordinate_name = model_backed
                                             ? *resource.source_joint_name
                                             : resource.compiled_resource_name;
    return CanonicalValue(CanonicalValue::Object{
        {"complete", CanonicalValue(true)},
        {"position",
         CoordinatePositionValue(observed_state->positions.front())},
        {"sequence",
         CanonicalValue(static_cast<double>(observed_state->sequence))},
        {"source_coordinate_name", CanonicalValue(source_coordinate_name)},
        {"source_epoch", CanonicalValue(observed_state->source_epoch.value())},
    });
  }

[[nodiscard]] CanonicalValue ExecutionCommandSurface::DirectControlGripperStatusValue(
      Composition &composition,
      const core::ObservedStateSnapshotDTO *observed_state) const {
    auto fields = DirectControlGripperTargetValue(composition).as_object();
    fields.emplace_back("observed_state", DirectControlGripperObservedValue(
                                              composition, observed_state));
    return CanonicalValue(std::move(fields));
  }

[[nodiscard]] ExecutionCommandSurface::CurrentControlState
  ExecutionCommandSurface::ReadCurrentControlState(Composition &composition) const {
    if (IsQualifiedProviderOnlyView(composition))
      return CurrentControlState{std::nullopt, std::nullopt};
    Require(composition.runtime != nullptr ||
                composition.gripper_runtime != nullptr,
            "selected component has no observed-state owner");
    const auto now = composition.runtime != nullptr
                         ? composition.runtime->clock->now()
                         : composition.cell_runtime->clock()->now();
    std::optional<core::ObservedStateSnapshotDTO> motion_state;
    if (composition.runtime != nullptr) {
      const auto motion =
          composition.runtime->motion_state_channel->current_state(now);
      if (!motion.ok())
        Fail("selected motion observed state is unavailable: " +
             motion.error().message);
      motion_state = motion.value();
    }
    const auto gripper_channel = composition.runtime != nullptr
                                     ? composition.runtime->gripper_state_channel
                                     : composition.gripper_runtime->state_channel();
    if (!composition.direct_control_gripper_binding.has_value()) {
      Require(gripper_channel == nullptr,
              "selected gripper observed-state channel has no binding");
      return CurrentControlState{std::move(motion_state), std::nullopt};
    }
    Require(gripper_channel != nullptr,
            "selected gripper observed-state channel is absent");
    const auto* gripper_backend = composition.runtime ? composition.runtime->gripper.get()
        : composition.gripper_runtime->backend();
    Require(gripper_backend != nullptr, "selected gripper has no configured backend");
    if (gripper_backend->gripper_actuation_kind(
            composition.direct_control_gripper_binding->value().command_target_id.value()) ==
        core::GripperActuationKind::kPneumatic)
      return CurrentControlState{std::move(motion_state), std::nullopt};
    Require(
        composition.direct_control_gripper_binding->value().resources.size() ==
            1U,
        "selected gripper observation has no exact scalar binding");
    const auto gripper = gripper_channel->current_state(now);
    const auto &resource =
        composition.direct_control_gripper_binding->value().resources.front();
    const bool component_local =
        !resource.source_joint_name.has_value() &&
        (composition.runtime != nullptr
             ? !composition.runtime->gripper_position_limits.has_value()
             : !composition.gripper_runtime->position_limits().has_value());
    const auto missing_reason =
        gripper.ok() ? std::string{}
                     : (gripper.error().details.contains("reason")
                            ? gripper.error().details.at("reason")
                            : std::string{});
    if (!gripper.ok() && (!component_local ||
                          missing_reason != "observed_state_not_published")) {
      Fail("selected gripper observed state is unavailable: " +
           gripper.error().message);
    }
    return CurrentControlState{
        std::move(motion_state),
        gripper.ok()
            ? std::optional<core::ObservedStateSnapshotDTO>{gripper.value()}
            : std::nullopt};
  }

[[nodiscard]] CanonicalValue::Object
  ExecutionCommandSurface::IdentityFields(ExecutionProduct &composition,
                 const core::ProjectionRevisionDTO &projection) const {
    const auto &bundle = *composition.bundle;
    Require(projection.retained_bundle_id ==
                    composition.projection_revision.retained_bundle_id &&
                projection.graph_revision ==
                    composition.projection_revision.graph_revision &&
                projection.scene_revision.valid(),
            "selected execution identity crosses its active product");
    return CanonicalValue::Object{
        {"active_publication_generation",
         CanonicalValue(
             static_cast<double>(composition.publication_generation.value()))},
        {"cell_id", CanonicalValue(bundle.value().cell_graph.cell_id.value())},
        {"control_management_mode",
         CanonicalValue(
             composition.cclc_attached.load(std::memory_order_acquire)
                 ? (composition.route_recovery_pending.load(
                        std::memory_order_acquire)
                        ? "pending"
                        : "component_lifecycle")
             : "pending")},
        {"graph_revision",
         CanonicalValue(projection.graph_revision.value())},
        {"owner_epoch", CanonicalValue(owner_epoch_.value())},
        {"retained_bundle_id",
         CanonicalValue(projection.retained_bundle_id.value())},
        {"scene_revision", SceneRevisionValue(projection.scene_revision)},
    };
  }

[[nodiscard]] CanonicalValue
  ExecutionCommandSurface::IdentityValue(ExecutionProduct &composition,
                const core::ProjectionRevisionDTO &projection) const {
    auto fields = IdentityFields(composition, projection);
    fields.emplace_back("schema_version",
                        CanonicalValue("manyforge.execution_identity.v2"));
    return CanonicalValue(std::move(fields));
  }

[[nodiscard]] CanonicalValue
  ExecutionCommandSurface::StatusResponse(Composition &composition,
                 const core::ProjectionRevisionDTO &projection) const {
    // Diagnostics are a cold-path view of the same three owner projections,
    // not another status contract with primary motion/gripper aliases.
    const auto targets = TargetsResponse(composition, projection);
    const auto control_state = ControlStateResponse(composition, projection);
    return CanonicalValue(CanonicalValue::Object{
        {"api_version", CanonicalValue(std::string(kResponseApiVersion))},
        {"command", CanonicalValue("status")},
        {"ok", CanonicalValue(true)},
        {"status", CanonicalValue(CanonicalValue::Object{
                       {"schema_version",
                        CanonicalValue("manyforge.execution_status.v2")},
                       {"identity", IdentityValue(composition, projection)},
                       {"targets", Member(targets, "targets")},
                       {"control_state", Member(control_state, "control_state")},
                   })},
    });
  }

[[nodiscard]] CanonicalValue
  ExecutionCommandSurface::IdentityResponse(ExecutionProduct &composition,
                   const core::ProjectionRevisionDTO &projection) const {
    return CanonicalValue(CanonicalValue::Object{
        {"api_version", CanonicalValue(std::string(kResponseApiVersion))},
        {"command", CanonicalValue("identity")},
        {"identity", IdentityValue(composition, projection)},
        {"ok", CanonicalValue(true)},
    });
  }

[[nodiscard]] CanonicalValue
  ExecutionCommandSurface::TargetsResponse(Composition &composition,
                  const core::ProjectionRevisionDTO &projection) const {
    Require(composition.bundle != nullptr &&
                composition.bundle->retained_bundle_id() ==
                    projection.retained_bundle_id &&
                composition.bundle->value().cell_graph.graph_revision ==
                    projection.graph_revision,
            "execution targets do not match the verified active bundle");
    struct TargetProjection final {
      std::string command_target_id;
      std::string operation_capability_id;
      CanonicalValue value;
    };
    struct ComponentProjection final {
      Composition *owner;
      std::vector<TargetProjection> targets;
    };
    using TargetKey = std::tuple<std::string, std::string, std::string>;
    std::map<core::ControllableComponentId, Composition *>
        runtime_component_owners;
    const auto register_runtime_component = [&](Composition &candidate) {
      Require(runtime_component_owners
                  .emplace(candidate.controllable_component_id, &candidate)
                  .second,
              "native execution component has multiple runtime owners");
    };
    register_runtime_component(composition);
    for (const auto &participant : composition.participant_compositions) {
      Require(participant != nullptr,
              "native execution component registry contains a missing owner");
      register_runtime_component(*participant);
    }

    std::map<core::ControllableComponentId, ComponentProjection>
        component_targets;
    std::set<core::ControllableComponentId> active_component_ids;
    for (const auto &component :
         composition.bundle->value().cell_graph.controllable_components) {
      Require(active_component_ids.insert(component.controllable_component_id)
                  .second,
              "verified active bundle contains a duplicate component identity");
      const auto runtime_owner =
          runtime_component_owners.find(component.controllable_component_id);
      auto [entry, inserted] = component_targets.try_emplace(
          component.controllable_component_id,
          ComponentProjection{runtime_owner == runtime_component_owners.end()
                                  ? nullptr
                                  : runtime_owner->second,
                              {}});
      static_cast<void>(entry);
      Require(inserted,
              "verified active bundle contains a duplicate target component row");
      Require(component_targets.size() <= 128U,
              "native execution target component list exceeds its bound");
    }
    for (const auto &[component_id, owner] : runtime_component_owners) {
      static_cast<void>(owner);
      Require(active_component_ids.contains(component_id),
              "native runtime registry contains a component outside the active bundle");
    }

    std::set<TargetKey> supported_target_keys;

    const auto append_target = [&](const CanonicalValue &source,
                                   const std::string_view presentation) {
      const auto component_id = core::ControllableComponentId(
          Text(source, "controllable_component_id"));
      const auto runtime_owner = runtime_component_owners.find(component_id);
      Require(runtime_owner != runtime_component_owners.end(),
              "execution target has no component in the runtime registry");
      const auto component = component_targets.find(component_id);
      Require(component != component_targets.end() &&
                  component->second.owner == runtime_owner->second,
              "supported execution target has no matching active runtime owner");

      const auto command_target_id = Text(source, "command_target_id");
      const auto operation_capability_id =
          Text(source, "operation_capability_id");
      Require(supported_target_keys
                  .insert({component_id.value(), command_target_id,
                           operation_capability_id})
                  .second,
              "native execution component has a duplicate supported target key");
      const auto field = [&](const std::string_view name) {
        return Member(source, name);
      };
      CanonicalValue::Object manual_control{
          {"program_binding_id", field("program_binding_id")},
          {"program_id", field("program_id")},
          {"program_revision", field("program_revision")},
          {"program_run_id", field("program_run_id")},
      };
      CanonicalValue::Object command;
      if (presentation == "motion") {
        command = {
            {"base_frame_role_id", field("base_frame_role_id")},
            {"component_role_id", field("component_role_id")},
            {"frame_role_ids", field("frame_role_ids")},
            {"joints", field("joints")},
            {"manual_control", CanonicalValue(std::move(manual_control))},
            {"planning_capability_ids", field("planning_capability_ids")},
            {"target_descriptor", field("target_descriptor")},
            {"target_role_id", field("target_role_id")},
            {"tcp_frame_role_id", field("tcp_frame_role_id")},
        };
        const auto *owner = runtime_owner->second;
        const auto stop =
            owner->target_resolver && owner->direct_control_motion_binding &&
                    owner->direct_control_motion_binding->value()
                            .command_target_id.value() == command_target_id &&
                    owner->direct_control_motion_binding->value()
                            .capability_id.value() == operation_capability_id
                ? owner->target_resolver->ReadQualifiedStopResponse(
                      *owner->direct_control_motion_binding)
                : core::Result<CanonicalValue>::Err(
                      {"stop_metadata_unavailable",
                       "The target has no exact product-backed Stop metadata "
                       "source",
                       {}});
        command.emplace_back("stop_response", stop.ok()
                                                  ? stop.value()
                                                  : CanonicalValue(nullptr));
        if (!stop.ok())
          command.emplace_back("stop_response_error", ErrorValue(stop.error()));
      } else {
        Require(presentation == "gripper",
                "native execution target has an unsupported presentation");
        command = {
            {"command_schema_id", field("command_schema_id")},
            {"command_schema_revision", field("command_schema_revision")},
            {"component_role_id", field("component_role_id")},
            {"manual_control", CanonicalValue(std::move(manual_control))},
            {"resource", field("resource")},
            {"target_descriptor", field("target_descriptor")},
            {"target_role_id", field("target_role_id")},
        };
      }
      CanonicalValue target(CanonicalValue::Object{
          {"command", CanonicalValue(std::move(command))},
          {"command_target_id", CanonicalValue(command_target_id)},
          {"operation_capability_id",
           CanonicalValue(operation_capability_id)},
          {"presentation", CanonicalValue(std::string(presentation))},
      });
      const auto *descriptor = target.find_object_member("command");
      Require(descriptor != nullptr,
              "native execution target has no command projection");
      const auto *target_descriptor =
          descriptor->find_object_member("target_descriptor");
      Require(target_descriptor != nullptr &&
                  Text(*target_descriptor, "controllable_component_id") ==
                      component_id.value() &&
                  Text(*target_descriptor, "command_target_id") ==
                      command_target_id,
              "execution target descriptor identity crosses its row key");
      component->second.targets.push_back(TargetProjection{
          command_target_id, operation_capability_id, std::move(target)});
    };

    const auto append_component_targets = [&](Composition &candidate) {
      if (candidate.direct_control_motion_binding.has_value()) {
        Require(candidate.runtime != nullptr &&
                    candidate.direct_control_joint_limits.has_value(),
                "native motion target has no installed motion owner or limits");
        append_target(DirectControlTargetValue(candidate), "motion");
      }
      if (candidate.direct_control_gripper_binding.has_value()) {
        Require(candidate.runtime != nullptr ||
                    candidate.gripper_runtime != nullptr,
                "native gripper target has no installed controller owner");
        append_target(DirectControlGripperTargetValue(candidate), "gripper");
      }
    };
    append_component_targets(composition);
    for (const auto &participant : composition.participant_compositions) {
      append_component_targets(*participant);
    }

    std::set<TargetKey> active_binding_keys;
    for (const auto &binding :
         composition.bundle->value().cell_graph.control_bindings) {
      const auto &component_id = binding.controllable_component_id;
      Require(active_component_ids.contains(component_id),
              "verified active control binding names a missing component");
      const TargetKey key{component_id.value(), binding.command_target_id.value(),
                          binding.capability_id.value()};
      Require(active_binding_keys.insert(key).second,
              "verified active bundle contains an ambiguous target key");
      if (supported_target_keys.contains(key)) {
        continue;
      }
      component_targets.at(component_id).targets.push_back(TargetProjection{
          binding.command_target_id.value(), binding.capability_id.value(),
          CanonicalValue(CanonicalValue::Object{
              {"command", CanonicalValue(nullptr)},
              {"command_target_id",
               CanonicalValue(binding.command_target_id.value())},
              {"operation_capability_id",
               CanonicalValue(binding.capability_id.value())},
              {"presentation", CanonicalValue("unsupported")},
          })});
    }
    for (const auto &key : supported_target_keys) {
      Require(active_binding_keys.contains(key),
              "native manual target is absent from the verified active bundle");
    }

    CanonicalValue::Array components;
    components.reserve(component_targets.size());
    for (auto &[component_id, component] : component_targets) {
      std::sort(component.targets.begin(), component.targets.end(),
                [](const auto &left, const auto &right) {
                  return std::tie(left.command_target_id,
                                  left.operation_capability_id) <
                         std::tie(right.command_target_id,
                                  right.operation_capability_id);
                });
      Require(std::adjacent_find(
                  component.targets.begin(), component.targets.end(),
                  [](const auto &left, const auto &right) {
                    return left.command_target_id == right.command_target_id &&
                           left.operation_capability_id ==
                               right.operation_capability_id;
                  }) == component.targets.end(),
              "native execution component has a duplicate addressed target");

      bool command_eligible = false;
      CanonicalValue reason("component_command_not_eligible");
      auto *kernel = component.owner == nullptr
                         ? nullptr
                         : component.owner->active_addressed_kernel();
      if (kernel != nullptr) {
        const auto readiness = kernel->component_command_readiness(component_id);
        if (!readiness.ok()) {
          const auto existing_reason = readiness.error().details.find("reason");
          if (existing_reason != readiness.error().details.end() &&
              !existing_reason->second.empty()) {
            reason = CanonicalValue(existing_reason->second);
          }
        } else if (readiness.value().controllable_component_id == component_id &&
                   readiness.value().component_lifecycle_generation.has_value()) {
          command_eligible = readiness.value().command_eligible;
          if (command_eligible) {
            reason = CanonicalValue(nullptr);
          }
        } else {
          reason = CanonicalValue("component_command_not_eligible");
        }
      } else {
        reason = CanonicalValue("component_readiness_kernel_missing");
      }

      CanonicalValue::Array targets;
      Require(component.targets.size() <= 128U,
              "native execution component target list exceeds its bound");
      targets.reserve(component.targets.size());
      for (auto &target : component.targets) {
        targets.emplace_back(std::move(target.value));
      }
      components.emplace_back(CanonicalValue::Object{
          {"availability",
           CanonicalValue(CanonicalValue::Object{
               {"command_eligible", CanonicalValue(command_eligible)},
               {"reason", std::move(reason)},
           })},
          {"controllable_component_id", CanonicalValue(component_id.value())},
          {"targets", CanonicalValue(std::move(targets))},
      });
    }

    CanonicalValue::Array io_bindings;
    std::vector<deployment::RetainedIoSignalConfiguration> io_catalog;
    if (!composition.direct_control_io_program_bindings.empty()) {
      auto catalog = deployment::ResolveRetainedIoSignalCatalog(
          composition.bundle->value().cell_graph);
      Require(catalog.ok(), "applied I/O source catalog is invalid");
      io_catalog = std::move(catalog.value());
    }
    for (const auto &binding : composition.direct_control_io_program_bindings) {
      Require(binding &&
                  binding->value().retained_bundle_id ==
                      projection.retained_bundle_id &&
                  binding->value().graph_revision == projection.graph_revision,
              "logical input binding differs from the active retained product");
      for (const auto &target :
           Array(binding->normalized_root(), "target_role_bindings")) {
        const auto *signals = target.find_object_member("signal_role_bindings");
        if (signals == nullptr || signals->as_array().empty())
          continue;
        CanonicalValue::Array sources;
        for (const auto &signal : signals->as_array()) {
          const auto source = std::find_if(
              io_catalog.begin(), io_catalog.end(), [&](const auto &row) {
                return row.descriptor.signal_id.controllable_component_id.value() ==
                           Text(target, "controllable_component_id") &&
                       row.command_target_id.value() ==
                           Text(target, "command_target_id") &&
                       row.resource_role_id.value() ==
                           Text(signal, "resource_role_id");
              });
          Require(source != io_catalog.end(),
                  "input binding has no exact applied source profile");
          sources.emplace_back(CanonicalValue::Object{
              {"resource_role_id", Member(signal, "resource_role_id")},
              {"source_kind", CanonicalValue(source->provider.profile)}});
        }
        io_bindings.emplace_back(CanonicalValue::Object{
            {"program_binding_id",
             CanonicalValue(binding->program_binding_id().value())},
            {"target_role_id", Member(target, "target_role_id")},
            {"controllable_component_id",
             Member(target, "controllable_component_id")},
            {"command_target_id", Member(target, "command_target_id")},
            {"signal_role_bindings", *signals},
            {"signal_sources", CanonicalValue(std::move(sources))}});
      }
    }
    std::sort(io_bindings.begin(), io_bindings.end(),
              [](const auto &left, const auto &right) {
                const auto key = [](const auto &row) {
                  return std::tuple{Text(row, "controllable_component_id"),
                                    Text(row, "command_target_id"),
                                    Text(row, "target_role_id"),
                                    Text(row, "program_binding_id")};
                };
                return key(left) < key(right);
              });
    CanonicalValue::Object target_fields{
        {"components", CanonicalValue(std::move(components))},
        {"schema_version", CanonicalValue("manyforge.execution_targets.v3")}};
    if (!io_bindings.empty())
      target_fields.emplace_back("io_signal_bindings",
                                 CanonicalValue(std::move(io_bindings)));
    return CanonicalValue(CanonicalValue::Object{
        {"api_version", CanonicalValue(std::string(kResponseApiVersion))},
        {"command", CanonicalValue("targets")},
        {"identity", IdentityValue(composition, projection)},
        {"ok", CanonicalValue(true)},
        {"targets", CanonicalValue(std::move(target_fields))},
    });
}

[[nodiscard]] CanonicalValue ExecutionCommandSurface::ControlStateResponse(
    Composition &composition,
    const core::ProjectionRevisionDTO &projection) const {
  const auto current = ReadCurrentControlState(composition);
  CanonicalValue runtime_scale_control_value(nullptr);
  if (composition.direct_control_motion_binding.has_value()) {
    const auto &motion_binding = composition.direct_control_motion_binding->value();
    const core::RuntimeScaleControlSelectorDTO runtime_scale_selector{
        motion_binding.program_binding_id, motion_binding.component_role_id,
        motion_binding.target_role_id};
    // This generic control projection shares the intrinsic manual target table.
    // Captured Program/group scaling has its separate explicit selector query.
    const auto runtime_scale_control =
        composition.active_addressed_kernel()->present_runtime_scale_control(
            runtime_scale_selector);
    if (!runtime_scale_control.ok()) {
      Fail("runtime-scale control projection failed: " +
           runtime_scale_control.error().message);
    }
    runtime_scale_control_value =
        RuntimeScaleControlStatusValue(runtime_scale_control.value());
  }
  const auto manual_operations =
      composition.active_addressed_kernel()->get_latest_run_operations(
          {direct_control_program_run_id_,
           direct_control_gripper_program_run_id_});
  CanonicalValue::Array manual_operation_values;
  manual_operation_values.reserve(manual_operations.size());
  for (const auto &projection : manual_operations) {
    manual_operation_values.emplace_back(CanonicalValue::Object{
        {"operation", OperationStateValue(projection.operation)},
        {"resident", CanonicalValue(projection.resident)}});
  }
  struct ObservedTargetRow final {
    std::tuple<std::string, std::string, std::string> key;
    CanonicalValue value;
  };
  std::vector<ObservedTargetRow> observed_rows;
  observed_rows.reserve(2U *
                        (1U + composition.participant_compositions.size()));
  const auto append_observed_target_state = [&](Composition &candidate,
                                                const CurrentControlState
                                                    &state) {
    if (candidate.direct_control_motion_binding.has_value()) {
      const auto &motion = candidate.direct_control_motion_binding->value();
      CanonicalValue motion_observed(nullptr);
      if (state.motion.has_value()) {
        auto observed =
            DirectControlObservedValue(candidate, *state.motion).as_object();
        observed.emplace_back("kind", CanonicalValue("joint_positions"));
        motion_observed = CanonicalValue(std::move(observed));
      }
      observed_rows.push_back(ObservedTargetRow{
          {motion.controllable_component_id.value(),
           motion.command_target_id.value(), motion.capability_id.value()},
          CanonicalValue(CanonicalValue::Object{
              {"controllable_component_id",
               CanonicalValue(motion.controllable_component_id.value())},
              {"command_target_id",
               CanonicalValue(motion.command_target_id.value())},
              {"operation_capability_id",
               CanonicalValue(motion.capability_id.value())},
              {"observed", std::move(motion_observed)},
          })});
    }
    if (candidate.direct_control_gripper_binding.has_value()) {
      const auto &gripper = candidate.direct_control_gripper_binding->value();
      CanonicalValue observed(nullptr);
      if (state.gripper.has_value()) {
        auto gripper_observed =
            DirectControlGripperObservedValue(candidate, &*state.gripper)
                .as_object();
        gripper_observed.emplace_back("kind", CanonicalValue("coordinate"));
        observed = CanonicalValue(std::move(gripper_observed));
      }
      observed_rows.push_back(ObservedTargetRow{
          {gripper.controllable_component_id.value(),
           gripper.command_target_id.value(), gripper.capability_id.value()},
          CanonicalValue(CanonicalValue::Object{
              {"controllable_component_id",
               CanonicalValue(gripper.controllable_component_id.value())},
              {"command_target_id",
               CanonicalValue(gripper.command_target_id.value())},
              {"operation_capability_id",
               CanonicalValue(gripper.capability_id.value())},
              {"observed", std::move(observed)},
          })});
    }
  };
  append_observed_target_state(composition, current);
  for (const auto &participant : composition.participant_compositions) {
    append_observed_target_state(*participant,
                                 ReadCurrentControlState(*participant));
  }
  std::sort(
      observed_rows.begin(), observed_rows.end(),
      [](const auto &left, const auto &right) { return left.key < right.key; });
  Require(std::adjacent_find(observed_rows.begin(), observed_rows.end(),
                             [](const auto &left, const auto &right) {
                               return left.key == right.key;
                             }) == observed_rows.end(),
          "control-state projection has duplicate observed target identities");
  CanonicalValue::Array observed_target_states;
  observed_target_states.reserve(observed_rows.size());
  for (auto &row : observed_rows)
    observed_target_states.emplace_back(std::move(row.value));
  return CanonicalValue(CanonicalValue::Object{
      {"api_version", CanonicalValue(std::string(kResponseApiVersion))},
      {"command", CanonicalValue("control_state")},
      {"control_state",
       CanonicalValue(CanonicalValue::Object{
           {"cell_stop_action",
            CellStopActionValue(composition.active_addressed_kernel()
                                    ->cell_stop_action_projection())},
           {"manual_operations",
            CanonicalValue(std::move(manual_operation_values))},
           {"observed_targets",
            CanonicalValue(std::move(observed_target_states))},
           {"runtime_scale_control", std::move(runtime_scale_control_value)},
           {"schema_version",
            CanonicalValue("manyforge.execution_control_state.v6")},
       })},
      {"identity", IdentityValue(composition, projection)},
      {"ok", CanonicalValue(true)},
  });
}

[[nodiscard]] ExecutionCommandSurface::ObservationProjection
  ExecutionCommandSurface::BuildObservationProjection(Composition &composition) const {
    std::shared_ptr<IRetainedObservedStateChannel> wake_channel;
    std::optional<std::uint64_t> wake_incarnation;
    const auto capture_wake_channel = [&](
        const std::shared_ptr<IRetainedObservedStateChannel> &channel) {
      Require(channel != nullptr,
              "active native component has no retained observation channel");
      const auto *render_channel =
          dynamic_cast<const RetainedObservedStateChannel *>(channel.get());
      Require(render_channel != nullptr,
              "native observation channel has no shared render wake identity");
      if (!wake_channel) {
        wake_channel = channel;
        wake_incarnation = render_channel->render_incarnation();
      } else {
        Require(wake_incarnation == render_channel->render_incarnation(),
                "native component observations do not share one cell render wake");
      }
    };

    struct ObservationRow final {
      std::tuple<std::string, std::string, std::string> key;
      CanonicalValue value;
    };
    // Apply publishes routes on the live product without the composition
    // fence. The channel is therefore the sole source of each target's route
    // binding: binding and sample are read together under its route fence,
    // never as an owner binding read followed by a later state read.
    struct ObservationSource final {
      std::shared_ptr<RetainedObservedStateChannel> channel;
      core::ControllableComponentId controllable_component_id;
      bool pneumatic_display{};
    };
    std::vector<ObservationSource> sources;
    sources.reserve(2U * (1U + composition.participant_compositions.size()));
    std::vector<ObservationRow> rows;
    rows.reserve(2U * (1U + composition.participant_compositions.size()));
    const auto append_target = [&](
        const std::shared_ptr<IRetainedObservedStateChannel> &selected_channel,
        const core::ControllableComponentId &owner_component_id,
        const bool pneumatic_display = false) {
      capture_wake_channel(selected_channel);
      sources.push_back(ObservationSource{
          std::static_pointer_cast<RetainedObservedStateChannel>(
              selected_channel),
          owner_component_id, pneumatic_display});
    };
    const auto project_target = [&](const ObservationSource &source) {
      if (source.pneumatic_display) {
        const auto render = source.channel->current_bound_render_state();
        if (!render.ok()) Fail("selected pneumatic display is unavailable: " + render.error().message);
        const auto& binding = render.value().binding.value();
        Require(binding.controllable_component_id == source.controllable_component_id &&
                    binding.control_path && binding.resources.size() == render.value().positions_si.size() &&
                    binding.resources.size() == render.value().compiled_joint_names.size(),
                "pneumatic display crosses its exact retained binding");
        CanonicalValue::Array names, compiled, kinds, positions;
        for (std::size_t i = 0; i < binding.resources.size(); ++i) {
          const auto& resource = binding.resources[i];
          Require(resource.source_joint_name && resource.compiled_resource_name ==
                      render.value().compiled_joint_names[i] && std::isfinite(render.value().positions_si[i]),
                  "pneumatic display has invalid retained coordinates");
          names.emplace_back(*resource.source_joint_name);
          compiled.emplace_back(resource.compiled_resource_name);
          kinds.emplace_back(resource.coordinate_kind == core::CoordinateKind::kAngular ? "angular" : "linear");
          positions.emplace_back(render.value().positions_si[i]);
        }
        rows.push_back(ObservationRow{
            {binding.controllable_component_id.value(), binding.command_target_id.value(), binding.capability_id.value()},
            CanonicalValue(CanonicalValue::Object{
                {"command_target_id", CanonicalValue(binding.command_target_id.value())},
                {"compiled_joint_names", CanonicalValue(std::move(compiled))},
                {"controllable_component_id", CanonicalValue(binding.controllable_component_id.value())},
                {"operation_capability_id", CanonicalValue(binding.capability_id.value())},
                {"coordinate_kinds", CanonicalValue(std::move(kinds))},
                {"positions_si", CanonicalValue(std::move(positions))},
                {"source_epoch", CanonicalValue("render." + std::to_string(render.value().render_incarnation))},
                {"source_joint_names", CanonicalValue(std::move(names))},
                {"source_sequence", CanonicalValue(static_cast<double>(render.value().render_generation))},
            })});
        return;
      }
      const auto now = composition.cell_runtime != nullptr
                           ? composition.cell_runtime->clock()->now()
                       : composition.runtime != nullptr &&
                                 composition.runtime->clock != nullptr
                           ? composition.runtime->clock->now()
                           : WallClock{}.now();
      const auto observed = source.channel->current_bound_state(now);
      if (!observed.ok())
        Fail("selected observation is unavailable: " +
             observed.error().message);
      const auto &binding = observed.value().binding.value();
      Require(binding.controllable_component_id ==
                      source.controllable_component_id &&
                  binding.control_path.has_value() &&
                  !binding.resources.empty(),
              "native observation target has no sealed control path or resources");
      const bool any_model_resource = std::any_of(
          binding.resources.begin(), binding.resources.end(),
          [](const auto &resource) {
            return resource.source_joint_name.has_value();
          });
      if (!any_model_resource)
        return;
      Require(std::all_of(binding.resources.begin(), binding.resources.end(),
                          [](const auto &resource) {
                            return resource.source_joint_name.has_value();
                          }),
              "native observation target mixes model and component-local resources");
      const auto &state = observed.value().state;
      const auto &path = *binding.control_path;
      std::vector<std::string_view> identity_mismatches;
      const auto check_identity = [&](const std::string_view field,
                                      const bool matches) {
        if (!matches) identity_mismatches.push_back(field);
      };
      check_identity("complete", state.complete);
      check_identity("resources", state.resources.size() ==
                                      binding.resources.size());
      check_identity("joint_names", state.joint_names.size() ==
                                        binding.resources.size());
      check_identity("positions", state.positions.size() ==
                                     binding.resources.size());
      check_identity("component_id", state.controllable_component_id ==
                                         binding.controllable_component_id);
      check_identity("command_target_id", state.command_target_id ==
                                               binding.command_target_id);
      check_identity("control_domain_id", state.control_domain_id ==
                                             path.control_domain_id);
      check_identity("control_domain_revision",
                     state.control_domain_revision ==
                         path.control_domain_revision);
      check_identity("control_binding_id", state.control_binding_id ==
                                               path.control_binding_id);
      check_identity("control_binding_revision",
                     state.control_binding_revision ==
                         path.control_binding_revision);
      check_identity("provider_instance_id", state.provider_instance_id ==
                                                 path.provider_instance_id);
      check_identity("provider_manifest_id", state.provider_manifest_id ==
                                                 path.provider_manifest_id);
      check_identity("provider_manifest_revision",
                     state.provider_manifest_revision ==
                         path.provider_manifest_revision);
      check_identity("controller_topology_id",
                     state.controller_topology_id ==
                         path.controller_topology_id);
      check_identity("topology_revision", state.topology_revision ==
                                               path.topology_revision);
      check_identity("control_route_id", state.control_route_id ==
                                              path.control_route_id);
      check_identity("active_control_configuration_id",
                     state.active_control_configuration_id ==
                         path.active_control_configuration_id);
      check_identity("active_control_configuration_generation",
                     state.active_control_configuration_generation ==
                         path.active_control_configuration_generation);
      check_identity("state_authority_id", state.state_authority_id ==
                                                path.state_authority_id);
      check_identity("state_authority_class", state.state_authority_class ==
                                                   path.state_authority_class);
      check_identity("source_epoch",
                     state.source_epoch == path.provider_runtime_epoch);
      if (!identity_mismatches.empty()) {
        std::string message =
            "selected observation crosses its retained target identity: ";
        for (const auto field : identity_mismatches) {
          if (message.back() != ' ') message.push_back(',');
          message.append(field);
        }
        message.append("; route configuration observed=");
        message.append(state.active_control_configuration_id.value());
        message.append(" expected=");
        message.append(path.active_control_configuration_id.value());
        message.append(" observed_generation=");
        message.append(std::to_string(
            state.active_control_configuration_generation.value()));
        message.append(" expected_generation=");
        message.append(std::to_string(
            path.active_control_configuration_generation.value()));
        Fail(std::move(message));
      }
      CanonicalValue::Array source_names, compiled_names, kinds, positions;
      source_names.reserve(binding.resources.size());
      compiled_names.reserve(binding.resources.size());
      kinds.reserve(binding.resources.size());
      positions.reserve(binding.resources.size());
      for (std::size_t index = 0U; index < binding.resources.size(); ++index) {
        const auto &resource = binding.resources[index];
        const auto *angular =
            std::get_if<core::AngularPositionDTO>(&state.positions[index]);
        const auto *linear =
            std::get_if<core::LinearPositionDTO>(&state.positions[index]);
        Require(resource.source_joint_name.has_value() &&
                    state.resources[index] == resource.resource_id &&
                    state.joint_names[index] ==
                        resource.compiled_resource_name &&
                    ((angular && resource.coordinate_kind ==
                                     core::CoordinateKind::kAngular) ||
                     (linear && resource.coordinate_kind ==
                                    core::CoordinateKind::kLinear)),
                "selected observation has invalid retained coordinates");
        const double value = angular ? angular->value_rad : linear->value_m;
        Require(std::isfinite(value),
                "selected observation position is not finite");
        source_names.emplace_back(*resource.source_joint_name);
        compiled_names.emplace_back(resource.compiled_resource_name);
        kinds.emplace_back(angular ? "angular" : "linear");
        positions.emplace_back(value);
      }
      rows.push_back(ObservationRow{
          {binding.controllable_component_id.value(),
           binding.command_target_id.value(), binding.capability_id.value()},
          CanonicalValue(CanonicalValue::Object{
              {"command_target_id",
               CanonicalValue(binding.command_target_id.value())},
              {"compiled_joint_names",
               CanonicalValue(std::move(compiled_names))},
              {"controllable_component_id",
               CanonicalValue(binding.controllable_component_id.value())},
              {"operation_capability_id",
               CanonicalValue(binding.capability_id.value())},
              {"coordinate_kinds", CanonicalValue(std::move(kinds))},
              {"positions_si", CanonicalValue(std::move(positions))},
              {"source_epoch", CanonicalValue(state.source_epoch.value())},
              {"source_joint_names", CanonicalValue(std::move(source_names))},
              {"source_sequence",
               CanonicalValue(static_cast<double>(state.sequence))},
          })});
    };

    const auto append_owner = [&](Composition &owner) {
      if (IsQualifiedProviderOnlyView(owner))
        return; // This qualified I/O view has no physical joint observation.
      Require((owner.runtime == nullptr) != (owner.gripper_runtime == nullptr),
              "native observation component has no unique runtime owner");
      if (owner.runtime != nullptr) {
        if (owner.direct_control_motion_binding.has_value()) {
          append_target(owner.runtime->motion_state_channel,
                        owner.controllable_component_id);
        }
        if (owner.direct_control_gripper_binding.has_value()) {
          append_target(owner.runtime->gripper_state_channel,
                        owner.controllable_component_id,
                        owner.runtime->gripper->gripper_actuation_kind(
                            owner.direct_control_gripper_binding->value().command_target_id.value()) ==
                            core::GripperActuationKind::kPneumatic);
        }
        Require(owner.direct_control_motion_binding.has_value() ||
                    owner.direct_control_gripper_binding.has_value(),
                "native component has no supported observed target");
        return;
      }
      Require(!owner.direct_control_motion_binding.has_value() &&
                  owner.direct_control_gripper_binding.has_value(),
              "gripper-only native owner has a non-gripper target");
      append_target(owner.gripper_runtime->state_channel(),
                    owner.controllable_component_id,
                    owner.gripper_runtime->backend()->gripper_actuation_kind(
                        owner.direct_control_gripper_binding->value().command_target_id.value()) ==
                        core::GripperActuationKind::kPneumatic);
    };

    append_owner(composition);
    for (const auto &participant : composition.participant_compositions) {
      Require(participant != nullptr,
              "native observation composition has a missing component owner");
      append_owner(*participant);
    }
    Require(wake_channel != nullptr && wake_incarnation.has_value(),
            "native cell has no owner observation wake source");
    // Read the cell-wide wake generation before taking the immutable source
    // snapshots. A later publication is either visible below or makes the
    // waiter predicate immediately true.
    const auto change_generation = wake_channel->render_change_generation();
    for (const auto &source : sources)
      project_target(source);
    const auto stream_epoch =
        "render." + std::to_string(*wake_incarnation);
    std::sort(rows.begin(), rows.end(), [](const auto &left, const auto &right) {
      return left.key < right.key;
    });
    Require(std::adjacent_find(rows.begin(), rows.end(),
                               [](const auto &left, const auto &right) {
                                 return left.key == right.key;
                               }) == rows.end(),
            "native cell observation repeats a target identity");
    CanonicalValue::Array targets;
    targets.reserve(rows.size());
    for (auto &row : rows)
      targets.emplace_back(std::move(row.value));
    const auto &bundle = *composition.bundle;
    auto response = CanonicalValue(CanonicalValue::Object{
        {"api_version", CanonicalValue(std::string(kResponseApiVersion))},
        {"command", CanonicalValue("observation")},
        {"observation",
         CanonicalValue(CanonicalValue::Object{
             {"cell_id",
              CanonicalValue(bundle.value().cell_graph.cell_id.value())},
             {"graph_revision",
              CanonicalValue(bundle.value().cell_graph.graph_revision.value())},
             {"owner_epoch", CanonicalValue(owner_epoch_.value())},
             {"retained_bundle_id",
              CanonicalValue(bundle.retained_bundle_id().value())},
             {"schema_version",
              CanonicalValue("manyforge.execution_observation.v3")},
             {"source_epoch", CanonicalValue(stream_epoch)},
             {"source_sequence",
              CanonicalValue(static_cast<double>(change_generation))},
             {"targets", CanonicalValue(std::move(targets))},
         })},
        {"ok", CanonicalValue(true)},
    });
    return ObservationProjection{std::move(response), std::move(wake_channel),
                                 stream_epoch, change_generation,
                                 change_generation};
  }

[[nodiscard]] CanonicalValue
  ExecutionCommandSurface::ObservationResponse(Composition &composition) const {
    return BuildObservationProjection(composition).response;
  }

[[nodiscard]] CanonicalValue
  ExecutionCommandSurface::ObservationResponseAfter(const Command &command, const int client_fd,
                           const std::stop_token stop_token) const {
    while (!stop_token.stop_requested()) {
      std::optional<ObservationProjection> projection;
      {
        // Keep the composition alive only while taking one immutable current
        // projection. Waiting never holds the Manager reconfiguration fence.
        std::shared_lock composition_lock(composition_access_.mutex);
        if (composition_access_.current == nullptr) {
          return ErrorResponse(
              "execution_owner_not_ready",
              "the managed execution owner has no active physical product",
              {{"owner_epoch", owner_epoch_.value()}});
        }
        projection.emplace(
            BuildObservationProjection(*composition_access_.current));
      }
      auto current = std::move(*projection);
      if (!command.observation_after_owner_epoch.has_value() ||
          command.observation_after_owner_epoch->value() !=
              owner_epoch_.value() ||
          *command.observation_after_source_epoch != current.source_epoch ||
          *command.observation_after_source_sequence <
              current.source_sequence) {
        return std::move(current.response);
      }
      if (*command.observation_after_source_sequence >
          current.source_sequence) {
        return ErrorResponse(
            "execution_observation_cursor_invalid",
            "the observation cursor is ahead of the current owner source");
      }

      while (!stop_token.stop_requested() &&
             !current.channel->wait_for_render_change(
                 current.change_generation, std::chrono::seconds(1))) {
        // The bounded wake is only a disconnect/shutdown check. A timeout
        // performs no owner read, projection, shaping, or encoding; only a
        // real source-generation change returns to the projection path.
        pollfd peer{client_fd, 0, 0};
        int peer_state = -1;
        do {
          peer_state = ::poll(&peer, 1, 0);
        } while (peer_state < 0 && errno == EINTR &&
                 !stop_token.stop_requested());
        if (peer_state < 0 ||
            (peer.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
          Fail("execution observation client disconnected");
        }
        if ((peer.revents & POLLIN) != 0) {
          char byte{};
          const auto count =
              ::recv(client_fd, &byte, 1, MSG_PEEK | MSG_DONTWAIT);
          if (count == 0) {
            Fail("execution observation client disconnected");
          }
        }
      }
    }
    Fail("execution observation source stopped");
  }

[[nodiscard]] CanonicalValue
  ExecutionCommandSurface::ExecuteStop(const Command &command,
              core::ResourceAddressedExecutionKernel &addressed_kernel) {
    if (command.kind == CommandKind::kStopProgramSelection) {
      const auto result =
          addressed_kernel.stop_program_selection(*command.program_selection);
      if (!result.ok())
        return ErrorResponse(result.error().code, result.error().message,
                             result.error().details);
      return CanonicalValue(CanonicalValue::Object{
          {"api_version", CanonicalValue(std::string(kResponseApiVersion))},
          {"command", CanonicalValue("stop_program_selection")},
          {"ok", CanonicalValue(true)},
          {"accepted",
           ProgramSelectionAcceptedValue(
               result.value(), command.program_selection->group_ids)}});
    }
    const bool ordinary = (command.kind == CommandKind::kStop ||
                           command.kind == CommandKind::kStopProgramRun) &&
                          command.stop.has_value();
    const bool full =
        command.kind == CommandKind::kFullStop && command.full_stop.has_value();
    const bool malformed = command.kind == CommandKind::kMalformedStop &&
                           command.malformed_stop.has_value();
    Require(static_cast<unsigned>(ordinary) + static_cast<unsigned>(full) +
                    static_cast<unsigned>(malformed) ==
                1U,
            "execution Stop control lane accepts one complete Stop command");
    const auto result = [&] {
      if (ordinary) {
        return addressed_kernel.stop(*command.stop);
      }
      if (full) {
        return addressed_kernel.full_stop(*command.full_stop);
      }
      return addressed_kernel.recognized_malformed_stop(
          core::MalformedStopIngressDTO{
              command.malformed_stop->request_id, owner_epoch_,
              command.malformed_stop->attribution,
              command.malformed_stop->recognized_route,
              command.malformed_stop->reason});
    }();
    if (!result.ok()) {
      return CanonicalValue(CanonicalValue::Object{
          {"api_version", CanonicalValue(std::string(kResponseApiVersion))},
          {"error", ErrorValue(result.error())},
          {"ok", CanonicalValue(false)},
      });
    }
    return CanonicalValue(CanonicalValue::Object{
        {"accepted", StopAcceptedValue(result.value())},
        {"api_version", CanonicalValue(std::string(kResponseApiVersion))},
        {"owner_epoch", CanonicalValue(owner_epoch_.value())},
        {"command",
         CanonicalValue(full ? "full_stop"
                        : command.kind == CommandKind::kStopProgramRun
                            ? "stop_program_run"
                            : "stop")},
        {"ok", CanonicalValue(true)},
    });
  }

[[nodiscard]] CanonicalValue
  ExecutionCommandSurface::ReadStop(const Command &command,
           core::ResourceAddressedExecutionKernel &addressed_kernel) const {
    const auto status = addressed_kernel.get_stop_status(*command.stop_id);
    if (!status.ok()) {
      return CanonicalValue(CanonicalValue::Object{
          {"api_version", CanonicalValue(std::string(kResponseApiVersion))},
          {"error", ErrorValue(status.error())},
          {"ok", CanonicalValue(false)},
      });
    }
    return CanonicalValue(CanonicalValue::Object{
        {"api_version", CanonicalValue(std::string(kResponseApiVersion))},
        {"command", CanonicalValue("read_stop")},
        {"ok", CanonicalValue(true)},
        {"stop_status", StopStatusSummaryValue(status.value())},
    });
  }

[[nodiscard]] CanonicalValue ExecutionCommandSurface::ReadStopFault(
      core::ResourceAddressedExecutionKernel &addressed_kernel) const {
    const auto offer = addressed_kernel.get_stop_fault_recovery_offer();
    if (!offer.ok()) {
      return CanonicalValue(CanonicalValue::Object{
          {"api_version", CanonicalValue(std::string(kResponseApiVersion))},
          {"error", ErrorValue(offer.error())},
          {"ok", CanonicalValue(false)},
      });
    }
    const auto global_offers = addressed_kernel.ReadGlobalIoRuleRecoveryOffers();
    if (!global_offers.ok())
      return ErrorResponse(global_offers.error().code, global_offers.error().message,
                           global_offers.error().details);
    CanonicalValue::Array rows;
    std::optional<core::InterruptPolicyId> previous;
    for (const auto &actual : global_offers.value()) {
      Require(actual.selector.context.owner_epoch == addressed_kernel.request_owner_epoch() &&
                  (!previous || *previous < actual.selector.rule_id),
              "global recovery offers differ from the actual owner/catalog order");
      rows.emplace_back(GlobalIoRuleRecoveryOfferValue(actual));
      previous = actual.selector.rule_id;
    }
    return CanonicalValue(CanonicalValue::Object{
        {"api_version", CanonicalValue(std::string(kResponseApiVersion))},
        {"command", CanonicalValue("get_stop_fault")},
        {"ok", CanonicalValue(true)},
        {"stop_fault", offer.value() ? StopFaultOfferValue(*offer.value())
                                     : CanonicalValue(nullptr)},
        {"global_rule_recovery_offers", CanonicalValue(std::move(rows))},
    });
  }

CanonicalValue ExecutionCommandSurface::RecoverGlobalIoRule(
    const core::IoGlobalRuleRecoveryIntentDTO &intent,
    core::ResourceAddressedExecutionKernel &owner) const {
  if (!manager_liveness_admission_.Allows(
          component_manager::detail::ManagerLivenessCommandClass::kEffectful))
    return ErrorResponse("manager_liveness_not_current",
                         "ordinary execution command requires an exact current "
                         "unfenced Manager lease");
  // Same actual product/callback lifetime lease as source processing. Core
  // recovery may arm a fresh successor; its deadline must be observed even
  // when no further input frame arrives after this successful operator action.
  std::shared_lock composition_lock(composition_access_.mutex);
  const auto result = owner.recover_global_io_rule(intent);
  if (!result.ok())
    return ErrorResponse(result.error().code, result.error().message,
                         result.error().details);
  if (result.value().accepted) {
    const auto *current = composition_access_.product();
    if (current && current->io_signal_runtime)
      current->io_signal_runtime->RequestRuleEvidenceProcessing();
  }
  return CanonicalValue(CanonicalValue::Object{
      {"api_version", CanonicalValue(std::string(kResponseApiVersion))},
      {"command", CanonicalValue("recover_stop_fault")},
      {"ok", CanonicalValue(true)},
      {"result", GlobalIoRuleRecoveryResultValue(result.value())}});
}

[[nodiscard]] CanonicalValue
ExecutionCommandSurface::ResetRobotPositionCapabilityResponse(
    Composition &composition, const core::ProjectionRevisionDTO &projection) const {
  auto capability = composition_access_.reset_robot_position_capability
      ? composition_access_.reset_robot_position_capability(composition)
      : core::Result<ProgramResetPositionCapability>::Ok(ProgramResetPositionCapability{
            false, "The active Deployment has no qualified simulator position-reset capability"});
  if (!capability.ok())
    return ErrorResponse(capability.error().code, capability.error().message,
                         capability.error().details);
  const auto &value = capability.value();
  Require(value.available == !value.reason.has_value(),
          "Reset position capability has an inconsistent reason");
  return CanonicalValue(CanonicalValue::Object{
      {"api_version", CanonicalValue(std::string(kResponseApiVersion))},
      {"command", CanonicalValue("reset_robot_position_capability")},
      {"ok", CanonicalValue(true)},
      {"identity", IdentityValue(composition, projection)},
      {"reset_robot_position", CanonicalValue(CanonicalValue::Object{
          {"available", CanonicalValue(value.available)},
          {"reason", value.reason ? CanonicalValue(*value.reason) : CanonicalValue(nullptr)}})}});
}

[[nodiscard]] CanonicalValue
ExecutionCommandSurface::ResetRobotPositionResponse(
    Composition &composition, const ProgramResetPositionRequest &request,
    const std::stop_token stop_token) const {
  if (!composition_access_.reset_robot_position)
    return ErrorResponse("program_cycle_reset_position_unavailable",
                         "The active Deployment has no qualified simulator position-reset capability",
                         {{"effect_disposition", "known_no_effect"}});
  const auto result = composition_access_.reset_robot_position(composition, request, stop_token);
  if (!result.ok())
    return ErrorResponse(result.error().code, result.error().message, result.error().details);
  return CanonicalValue(CanonicalValue::Object{
      {"api_version", CanonicalValue(std::string(kResponseApiVersion))},
      {"command", CanonicalValue("reset_robot_position")}, {"ok", CanonicalValue(true)}});
}

[[nodiscard]] CanonicalValue
ExecutionCommandSurface::EventsResponse(const Command &command) const {
  bool execution_owner_ready;
  {
    // Reuse the existing composition-selection authority, only long enough
    // to copy readiness. Journal I/O and fault reads never hold this lock.
    std::shared_lock composition_lock(composition_access_.mutex);
    execution_owner_ready = composition_access_.product() != nullptr;
  }
  auto &owner = StableKernel();
  // Copy owner facts before any journal I/O. The stable owner and journal
  // outlive product composition; a failed page cannot erase these facts.
  const auto actual_owner_epoch = owner.request_owner_epoch();
  CanonicalValue::Object metadata{
      {"api_version", CanonicalValue(std::string(kResponseApiVersion))},
      {"command", CanonicalValue("events")},
      {"execution_owner_ready", CanonicalValue(execution_owner_ready)},
      {"owner_epoch", CanonicalValue(actual_owner_epoch.value())},
      {"runtime_faults", RuntimeFaultsValue(owner)},
  };
  const auto failure = [&](const core::ErrorDTO &error) {
    // Page diagnostics can contain arbitrary backend/exception text. Use the
    // same bounded canonical projection as the retained Kernel write fault.
    core::ErrorDTO diagnostic{
        core::bounded_operation_journal_diagnostic_text(error.code, 256U),
        core::bounded_operation_journal_diagnostic_text(error.message, 1024U), {}};
    for (const auto &[key, value] : error.details) {
      if (diagnostic.details.size() >= 8U) break;
      // Encoding can make distinct source keys identical. Keep the first
      // diagnostic rather than creating invalid duplicate JSON members.
      diagnostic.details.emplace(
          core::bounded_operation_journal_diagnostic_text(key, 64U),
          core::bounded_operation_journal_diagnostic_text(value, 256U));
    }
    auto response = metadata;
    response.emplace_back("error", ErrorValue(diagnostic));
    response.emplace_back("ok", CanonicalValue(false));
    return CanonicalValue(std::move(response));
  };
  try {
    // One same-lock owner read supplies catalog identity AND pending/ready
    // baseline. Keep both independent of page I/O and projection progress.
    auto global_fields = InterruptRuleEventMetadataFields(
        owner.ReadInterruptRuleStateBaseline(), actual_owner_epoch);
    for (auto &field : global_fields) metadata.push_back(std::move(field));
    auto *const journal = composition_access_.operation_journal.load();
    Require(journal != nullptr,
            "operational events have no stable cell journal owner");
    const auto page = journal->ReadOperationalEvents(
        *command.event_owner_epoch, command.after_sequence, command.event_limit);
    if (!page.ok()) return failure(page.error());
    const auto admission = owner.GetAuthoringAdmission();
    // Progress remains presentation-only on this same existing read.
    CanonicalValue::Array operation_progress;
    for (const auto &operation : owner.get_resident_operations(
             {direct_control_program_run_id_, direct_control_gripper_program_run_id_})) {
      if (operation.state != core::OperationStateKind::kExecuting ||
          operation.participants.size() != 1U)
        continue;
      operation_progress.emplace_back(CanonicalValue::Object{
          {"owner_epoch", CanonicalValue(actual_owner_epoch.value())},
          {"operation_id", CanonicalValue(operation.operation_id.value())},
          {"operation_revision", CanonicalValue(static_cast<double>(
                                     operation.operation_revision.value()))},
          {"progress", CanonicalValue(operation.participants.front().progress)},
      });
    }
    metadata.emplace_back("admission_generation", CanonicalValue(
        static_cast<double>(admission.admission_generation.value())));
    metadata.emplace_back("event_page", OperationEventPageValue(page.value()));
    metadata.emplace_back("operation_progress", CanonicalValue(std::move(operation_progress)));
    metadata.emplace_back("ok", CanonicalValue(true));
    return CanonicalValue(std::move(metadata));
  } catch (const std::exception &error) {
    // Match the ordinary command error without losing the independent facts.
    return failure(core::ErrorDTO{"execution_command_invalid", error.what(), {}});
  } catch (...) {
    return failure(core::ErrorDTO{"execution_command_invalid",
                                 "operational event page read threw", {}});
  }
}

std::optional<CanonicalValue> ExecutionCommandSurface::ExecuteProgramCommand(
    const Command &command, ExecutionProduct &product,
    core::ResourceAddressedExecutionKernel *owner) {
    if (command.kind == CommandKind::kSetMockIoRunPaused) {
      Require(owner != nullptr && command.mock_run_simulation_pause.has_value(),
              "Run Mock simulation clock has no stable Kernel or request");
      return MockRunPauseResponse(*command.mock_run_simulation_pause, *owner);
    }
    if (command.kind == CommandKind::kEndMockIoNodeSimulation) {
      Require(owner != nullptr && command.mock_node_simulation_end.has_value(), "Mock simulation end has no stable Kernel or scope");
      return MockNodeSimulationResponse(*command.mock_node_simulation_end, nullptr, product, *owner);
    }
    if (command.kind == CommandKind::kStartMockIoNodeSimulation) {
      Require(owner != nullptr, "Mock simulation has no stable Kernel owner");
      return MockNodeSimulationResponse(command.mock_node_simulation->scope,
                                        &command.mock_node_simulation->signals, product, *owner);
    }
    if (command.kind == CommandKind::kReadProgramExecutionScope) {
      const auto result =
          owner
              ->get_program_execution_scope(
              *command.program_run_id);
      if (!result.ok())
        return ErrorResponse(result.error().code, result.error().message,
                             result.error().details);
      return CanonicalValue(CanonicalValue::Object{
          {"api_version", CanonicalValue(std::string(kResponseApiVersion))},
          {"command", CanonicalValue("read_program_execution_scope")},
          {"ok", CanonicalValue(true)},
          {"scope", ProgramExecutionScopeValue(result.value())}});
    }
    if (command.kind == CommandKind::kRegisterProgramCapture) {
      const auto capture = product.target_resolver
          ? product.target_resolver->ResolveProgramCapture(*command.program_capture)
          : detail::ResolveSealedProgramCapture(*command.program_capture,
                product.program, product.program_binding, *product.bundle);
      const auto result =
          capture.ok() ? owner
                             ->register_program_run_capture(capture.value())
                       : core::Result<void>::Err(capture.error());
      if (!result.ok()) {
        return CanonicalValue(CanonicalValue::Object{
            {"api_version", CanonicalValue(std::string(kResponseApiVersion))},
            {"error", ErrorValue(result.error())},
            {"ok", CanonicalValue(false)},
        });
      }
      return CanonicalValue(CanonicalValue::Object{
          {"api_version", CanonicalValue(std::string(kResponseApiVersion))},
          {"command", CanonicalValue("register_program_run_capture")},
          {"ok", CanonicalValue(true)},
          {"program_run_id",
           CanonicalValue(capture.value().program_run_id.value())},
          {"claim_generation", CanonicalValue(static_cast<double>(
                                   capture.value().claim_generation.value()))},
      });
    }
    if (command.kind == CommandKind::kStartProgramSelection ||
        command.kind == CommandKind::kReleaseProgramSelection) {
      const auto result =
          command.kind == CommandKind::kStartProgramSelection
              ? owner
                    ->start_program_selection(
                    *command.program_selection, command.expected_selection_revision)
              : owner
                    ->release_program_selection(
                    *command.program_selection, command.expected_selection_revision);
      if (!result.ok())
        return ErrorResponse(result.error().code, result.error().message,
                             result.error().details);
      return CanonicalValue(CanonicalValue::Object{
          {"api_version", CanonicalValue(std::string(kResponseApiVersion))},
          {"command", CanonicalValue(
                          command.kind == CommandKind::kStartProgramSelection
                              ? "start_program_selection"
                              : "release_program_selection")},
          {"ok", CanonicalValue(true)},
          {"scope", ProgramExecutionScopeValue(result.value())}});
    }
    if (command.kind == CommandKind::kCheckProgramNodeMotionAttention) {
      const auto result =
          owner
              ->check_program_node_motion_attention(
                  *command.program_node_attention);
      if (!result.ok())
        return ErrorResponse(result.error().code, result.error().message,
                             result.error().details);
      return CanonicalValue(CanonicalValue::Object{
          {"api_version", CanonicalValue(std::string(kResponseApiVersion))},
          {"command",
           CanonicalValue("check_program_node_motion_attention")},
          {"ok", CanonicalValue(true)}});
    }
    if (command.kind == CommandKind::kReleaseProgramRunStopFence) {
      const auto result =
          owner
              ->release_program_run_stop_fence(
              *command.program_run_id, *command.stop_id,
              *command.stop_generation);
      if (!result.ok()) {
        return CanonicalValue(CanonicalValue::Object{
            {"api_version", CanonicalValue(std::string(kResponseApiVersion))},
            {"error", ErrorValue(result.error())},
            {"ok", CanonicalValue(false)},
        });
      }
      return CanonicalValue(CanonicalValue::Object{
          {"api_version", CanonicalValue(std::string(kResponseApiVersion))},
          {"command", CanonicalValue("release_program_run_stop_fence")},
          {"ok", CanonicalValue(true)},
      });
    }
    return std::nullopt;
}

CanonicalValue ExecutionCommandSurface::ExecuteLogicalProduct(
    const Command &command, ExecutionProduct &product) {
  Require(product.bundle &&
              product.bundle->value().cell_graph.controllable_components.empty(),
          "logical command selected a physical component product");
  if (auto result = ExecuteProgramCommand(command, product, &StableKernel()))
    return std::move(*result);
  const auto &projection = product.projection_revision;
  if (command.kind == CommandKind::kIdentity)
    return IdentityResponse(product, projection);
  const auto identity = IdentityValue(product, projection);
  const auto targets = CanonicalValue(CanonicalValue::Object{
      {"components", CanonicalValue(CanonicalValue::Array{})},
      {"schema_version", CanonicalValue("manyforge.execution_targets.v3")}});
  CanonicalValue::Array manual_operations;
  for (const auto &actual : StableKernel().get_latest_run_operations(
           {direct_control_program_run_id_, direct_control_gripper_program_run_id_}))
    manual_operations.emplace_back(CanonicalValue::Object{
        {"operation", OperationStateValue(actual.operation)},
        {"resident", CanonicalValue(actual.resident)}});
  const auto control = CanonicalValue(CanonicalValue::Object{
      {"cell_stop_action", CellStopActionValue(StableKernel().cell_stop_action_projection())},
      {"manual_operations", CanonicalValue(std::move(manual_operations))},
      {"observed_targets", CanonicalValue(CanonicalValue::Array{})},
      {"runtime_scale_control", CanonicalValue(nullptr)},
      {"schema_version", CanonicalValue("manyforge.execution_control_state.v6")}});
  if (command.kind == CommandKind::kTargets)
    return CanonicalValue(CanonicalValue::Object{
        {"api_version", CanonicalValue(std::string(kResponseApiVersion))},
        {"command", CanonicalValue("targets")}, {"identity", identity},
        {"ok", CanonicalValue(true)}, {"targets", targets}});
  if (command.kind == CommandKind::kControlState)
    return CanonicalValue(CanonicalValue::Object{
        {"api_version", CanonicalValue(std::string(kResponseApiVersion))},
        {"command", CanonicalValue("control_state")}, {"identity", identity},
        {"ok", CanonicalValue(true)}, {"control_state", control}});
  if (command.kind == CommandKind::kStatus)
    return CanonicalValue(CanonicalValue::Object{
        {"api_version", CanonicalValue(std::string(kResponseApiVersion))},
        {"command", CanonicalValue("status")}, {"ok", CanonicalValue(true)},
        {"status", CanonicalValue(CanonicalValue::Object{
            {"schema_version", CanonicalValue("manyforge.execution_status.v2")},
            {"identity", identity}, {"targets", targets}, {"control_state", control}})}});
  // Effect dispatch continues to require an actual physical Composition.
  return OwnerNotReadyResponse(command);
}

[[nodiscard]] CanonicalValue
  ExecutionCommandSurface::Execute(const Command &command, Composition &composition,
          const core::ProjectionRevisionDTO &projection,
          const std::stop_token planning_stop_token) {
    if (auto result = ExecuteProgramCommand(command, composition,
                                             composition.active_addressed_kernel()))
      return std::move(*result);
    if (command.kind == CommandKind::kStatus) {
      return StatusResponse(composition, projection);
    }
    if (command.kind == CommandKind::kIdentity) {
      return IdentityResponse(composition, projection);
    }
    if (command.kind == CommandKind::kResetRobotPositionCapability) {
      return ResetRobotPositionCapabilityResponse(composition, projection);
    }
    if (command.kind == CommandKind::kResetRobotPosition) {
      return ResetRobotPositionResponse(composition, *command.reset_robot_position, planning_stop_token);
    }
    if (command.kind == CommandKind::kTargets) {
      return TargetsResponse(composition, projection);
    }
    if (command.kind == CommandKind::kControlState) {
      return ControlStateResponse(composition, projection);
    }
    if (command.kind == CommandKind::kObservation) {
      return ObservationResponse(composition);
    }
    if (command.kind == CommandKind::kStop ||
        command.kind == CommandKind::kStopProgramRun ||
        command.kind == CommandKind::kStopProgramSelection ||
        command.kind == CommandKind::kFullStop ||
        command.kind == CommandKind::kMalformedStop) {
      return ExecuteStop(command,
                         *composition.active_addressed_kernel());
    }
    if (command.kind == CommandKind::kSetMockIoInput)
      return MockIoInputResponse(*command.mock_io_input_set, composition);
    const bool read_only = command.kind == CommandKind::kQueryTarget ||
                           command.kind == CommandKind::kQueryOperation ||
                           command.kind == CommandKind::kObservation ||
                           command.kind == CommandKind::kRead ||
                           command.kind == CommandKind::kGetOperationRecovery ||
                           command.kind == CommandKind::kGetRuntimeScaleControl ||
                           command.kind ==
                               CommandKind::kCheckProgramNodeMotionAttention;
    const auto liveness_command =
        read_only
            ? component_manager::detail::ManagerLivenessCommandClass::kReadOnly
            : component_manager::detail::ManagerLivenessCommandClass::
                  kEffectful;
    if (!manager_liveness_admission_.Allows(liveness_command)) {
      return ErrorResponse(
          "manager_liveness_not_current",
          "ordinary execution command requires an exact current unfenced "
          "Manager lease");
    }
    if (command.kind == CommandKind::kQueryTarget) {
      return QueryTargetResponse(*command.target_query, composition);
    }
    if (command.kind == CommandKind::kQueryOperation) {
      return QueryOperationResponse(*command.operation_query, composition);
    }
    if (command.kind == CommandKind::kRecoverStopFault) {
      if (command.global_io_rule_recovery)
        return RecoverGlobalIoRule(*command.global_io_rule_recovery,
                                   *composition.active_addressed_kernel());
      Require(command.stop_fault_recovery.has_value(),
              "Stop-fault recovery command has no complete intent");
      const auto result =
          composition.active_addressed_kernel()->recover_stop_fault(
              *command.stop_fault_recovery);
      if (!result.ok()) {
        return CanonicalValue(CanonicalValue::Object{
            {"api_version", CanonicalValue(std::string(kResponseApiVersion))},
            {"error", ErrorValue(result.error())},
            {"ok", CanonicalValue(false)},
        });
      }
      return CanonicalValue(CanonicalValue::Object{
          {"api_version", CanonicalValue(std::string(kResponseApiVersion))},
          {"command", CanonicalValue("recover_stop_fault")},
          {"ok", CanonicalValue(true)},
          {"result", StopFaultRecoveryResultValue(result.value())},
      });
    }
    if (command.kind == CommandKind::kRead) {
      const auto projection =
          composition.active_addressed_kernel()
              ->get_operation_projection(
              *command.operation_id);
      if (!projection.ok()) {
        return CanonicalValue(CanonicalValue::Object{
            {"api_version", CanonicalValue(std::string(kResponseApiVersion))},
            {"error", ErrorValue(projection.error())},
            {"ok", CanonicalValue(false)},
        });
      }
      return CanonicalValue(CanonicalValue::Object{
          {"api_version", CanonicalValue(std::string(kResponseApiVersion))},
          {"command", CanonicalValue("read")},
          {"ok", CanonicalValue(true)},
          {"owner_epoch",
           CanonicalValue(projection.value().request_owner_epoch.value())},
          {"operation", OperationStateValue(projection.value().operation)},
          {"resident", CanonicalValue(projection.value().resident)},
          {"last_control",
           projection.value().last_control.has_value()
               ? ControlAcceptedValue(*projection.value().last_control)
               : CanonicalValue(nullptr)},
          {"control_pending",
           CanonicalValue(projection.value().control_pending)},
      });
    }
    if (command.kind == CommandKind::kGetOperationRecovery) {
      const auto assessment =
          composition.active_addressed_kernel()
              ->get_operation_recovery_assessment(*command.operation_id);
      if (!assessment.ok()) {
        return CanonicalValue(CanonicalValue::Object{
            {"api_version", CanonicalValue(std::string(kResponseApiVersion))},
            {"error", ErrorValue(assessment.error())},
            {"ok", CanonicalValue(false)},
        });
      }
      return CanonicalValue(CanonicalValue::Object{
          {"api_version", CanonicalValue(std::string(kResponseApiVersion))},
          {"command", CanonicalValue("get_operation_recovery")},
          {"ok", CanonicalValue(true)},
          {"operation_revision",
           CanonicalValue(static_cast<double>(
               assessment.value().operation_revision.value()))},
          {"stop_generation",
           assessment.value().stop_generation.has_value()
               ? CanonicalValue(static_cast<double>(
                     assessment.value().stop_generation->value()))
               : CanonicalValue(nullptr)},
          {"stop_id", assessment.value().stop_id.has_value()
                          ? CanonicalValue(assessment.value().stop_id->value())
                          : CanonicalValue(nullptr)},
          {"participants",
           RecoveryParticipantsValue(assessment.value().participants)},
          {"recovery_assessment",
           RecoveryAssessmentValue(assessment.value().assessment)},
      });
    }
    const auto reconcile_error = LastReconcileError();
    if (reconcile_error.has_value()) {
      return CanonicalValue(CanonicalValue::Object{
          {"api_version", CanonicalValue(std::string(kResponseApiVersion))},
          {"error", ErrorValue(*reconcile_error)},
          {"ok", CanonicalValue(false)},
      });
    }
    if (command.kind == CommandKind::kPreviewCapabilityCommand) {
      auto preview =
          composition.active_addressed_kernel()
              ->preview_capability_command(
              *command.operation);
      if (!preview.ok()) {
        return CanonicalValue(CanonicalValue::Object{
            {"api_version", CanonicalValue(std::string(kResponseApiVersion))},
            {"error", ErrorValue(preview.error())},
            {"ok", CanonicalValue(false)},
        });
      }
      auto *const target_composition =
          FindBindingComposition(preview.value().binding, composition);
      if (target_composition == nullptr) {
        return ErrorResponse(
            "execution_target_observation_unavailable",
            "capability preview has no unique qualified target owner");
      }
      return CanonicalValue(CanonicalValue::Object{
          {"api_version", CanonicalValue(std::string(kResponseApiVersion))},
          {"command", CanonicalValue("preview_capability_command")},
          {"ok", CanonicalValue(true)},
          {"preview_receipt",
           CapabilityCommandPreviewReceiptValue(
               preview.value(), *command.operation, *command.operation_digest,
               target_composition->runtime
                   ? target_composition->runtime->gripper_position_limits
                   : target_composition->gripper_runtime->position_limits(),
               owner_epoch_)},
      });
    }
    if (command.kind == CommandKind::kReadManualHandover) {
      const auto result =
          composition.active_addressed_kernel()
              ->get_manual_control_availability(*command.operation);
      if (!result.ok()) {
        return CanonicalValue(CanonicalValue::Object{
            {"api_version", CanonicalValue(std::string(kResponseApiVersion))},
            {"error", ErrorValue(result.error())},
            {"ok", CanonicalValue(false)}});
      }
      CanonicalValue handover(nullptr);
      if (result.value().handover) {
        const auto &held = *result.value().handover;
        const auto &capture = held.capture;
        CanonicalValue::Array indices;
        indices.reserve(held.producer_node_indices.size());
        for (const auto index : held.producer_node_indices)
          indices.emplace_back(static_cast<double>(index));
        handover = CanonicalValue(CanonicalValue::Object{
            {"context",
             CanonicalValue(CanonicalValue::Object{
                 {"claim_generation", CanonicalValue(static_cast<double>(
                                          capture.claim_generation.value()))},
                 {"program_run_id",
                  CanonicalValue(capture.program_run_id.value())},
                 {"program_runner_owner_epoch",
                  CanonicalValue(capture.program_runner_owner_epoch.value())},
                 {"selection_revision", CanonicalValue(static_cast<double>(
                                            held.selection_revision))}})},
            {"producer_node_indices", CanonicalValue(std::move(indices))}});
      }
      return CanonicalValue(CanonicalValue::Object{
          {"api_version", CanonicalValue(std::string(kResponseApiVersion))},
          {"command", CanonicalValue("read_manual_handover")},
          {"availability",
           CanonicalValue(CanonicalValue::Object{
               {"available", CanonicalValue(result.value().available)},
               {"cleanup_required",
                CanonicalValue(result.value().cleanup_required)},
               {"unavailable_reason",
                result.value().unavailable_reason
                    ? CanonicalValue(*result.value().unavailable_reason)
                    : CanonicalValue(nullptr)}})},
          {"handover", std::move(handover)},
          {"intent_digest", CanonicalValue(command.operation_digest->value())},
          {"ok", CanonicalValue(true)},
          {"request_id",
           CanonicalValue(
               command.operation->request_context.request_id.value())},
          {"request_owner_epoch", CanonicalValue(owner_epoch_.value())}});
    }
    if (command.kind == CommandKind::kPreviewMotion) {
      if (composition.runtime == nullptr) {
        return ErrorResponse(
            "motion_target_unavailable",
            "motion preview requires an installed motion-capable component");
      }
      auto preview_intent = *command.operation;
      const auto predecessor = composition.runtime->collision_publisher
                                   ->current_scene_snapshot_lease_inventory();
      // A preview receipt is explanatory evidence, not authority (spec 210
      // §3.10.1): the client's status read may lag the owner-minted,
      // monotonic Scene revision. Accept an earlier revision of the same
      // Scene and compute on the current one; the receipt names the revision
      // actually used. A future revision or a different Scene is refused.
      if (!predecessor.has_value() ||
          !preview_intent.expected_scene_revision.has_value() ||
          preview_intent.expected_scene_revision->scene_id !=
              predecessor->projection_binding.projection_revision
                  .scene_revision.scene_id ||
          preview_intent.expected_scene_revision->revision_id >
              predecessor->projection_binding.projection_revision
                  .scene_revision.revision_id ||
          // An equal revision id names exactly the current revision.
          (preview_intent.expected_scene_revision->revision_id ==
               predecessor->projection_binding.projection_revision
                   .scene_revision.revision_id &&
           *preview_intent.expected_scene_revision !=
               predecessor->projection_binding.projection_revision
                   .scene_revision)) {
        return ErrorResponse(
            "scene_changed_during_validation",
            "motion preview does not name the pre-latch scene revision");
      }
      preview_intent.expected_scene_revision =
          predecessor->projection_binding.projection_revision.scene_revision;
      if (composition.runtime->planning_scene_synchronizer != nullptr) {
        auto synchronized = composition.runtime->planning_scene_synchronizer
                                ->SynchronizeBeforePlanning();
        if (!synchronized.ok()) {
          return CanonicalValue(CanonicalValue::Object{
              {"api_version", CanonicalValue(std::string(kResponseApiVersion))},
              {"error", ErrorValue(synchronized.error())},
              {"ok", CanonicalValue(false)},
          });
        }
        preview_intent.expected_scene_revision =
            synchronized.value().scene_revision;
      }
      const auto preview_lease_id = core::ProjectionLeaseId{
          "motion-preview." +
          preview_intent.request_context.request_id.value()};
      auto inventory =
          composition.runtime->collision_publisher
              ->retain_scene_snapshot_lease_inventory(preview_lease_id);
      if (!inventory.has_value()) {
        return ErrorResponse(
            "motion_preview_projection_unavailable",
            "the native owner could not retain an exact collision projection");
      }
      CollisionLeaseRelease release(
          *composition.runtime->collision_publisher,
          inventory->projection_binding.projection_lease_id);
      auto session =
          composition.active_addressed_kernel()
              ->begin_motion_preview_session(
              preview_intent);
      if (!session.ok()) {
        return CanonicalValue(CanonicalValue::Object{
            {"api_version", CanonicalValue(std::string(kResponseApiVersion))},
            {"error", ErrorValue(session.error())},
            {"ok", CanonicalValue(false)},
        });
      }
      auto preview_binding = session.value()->resolve_binding(preview_intent);
      if (!preview_binding.ok()) {
        return CanonicalValue(CanonicalValue::Object{
            {"api_version", CanonicalValue(std::string(kResponseApiVersion))},
            {"error", ErrorValue(preview_binding.error())},
            {"ok", CanonicalValue(false)},
        });
      }
      auto start_state = session.value()->current_state();
      if (!start_state.ok()) {
        return CanonicalValue(CanonicalValue::Object{
            {"api_version", CanonicalValue(std::string(kResponseApiVersion))},
            {"error", ErrorValue(start_state.error())},
            {"ok", CanonicalValue(false)},
        });
      }
      auto preview = session.value()->preview_motion(
          preview_intent, std::move(*inventory),
          std::move(start_state.value()), 0U, planning_stop_token);
      if (!preview.ok()) {
        return CanonicalValue(CanonicalValue::Object{
            {"api_version", CanonicalValue(std::string(kResponseApiVersion))},
            {"error", ErrorValue(preview.error())},
            {"ok", CanonicalValue(false)},
        });
      }
      return CanonicalValue(CanonicalValue::Object{
          {"api_version", CanonicalValue(std::string(kResponseApiVersion))},
          {"command", CanonicalValue("preview_motion")},
          {"ok", CanonicalValue(true)},
          {"preview_receipt",
           MotionPreviewReceiptValue(preview.value(), *command.operation,
                                     *command.operation_digest,
                                     preview_binding.value(), owner_epoch_)},
      });
    }
    if (command.kind == CommandKind::kPlanPick ||
        command.kind == CommandKind::kPlanPlace) {
      if (!composition.pick_place_planning_owner) {
        return ErrorResponse(
            "pick_place_planning_owner_unavailable",
            "the selected native process has no joined logical-scene "
            "planning owner");
      }
      const auto sequence_id = command.kind == CommandKind::kPlanPick
                                   ? command.pick_planning->sequence_id
                                   : command.place_planning->sequence_id;
      if (command.kind == CommandKind::kPlanPick) {
        if (auto verified = VerifyPickReferenceTargets(
                *composition.target_resolver, *command.pick_planning);
            !verified.ok()) {
          return CanonicalValue(CanonicalValue::Object{
              {"api_version", CanonicalValue(std::string(kResponseApiVersion))},
              {"error", ErrorValue(verified.error())},
              {"ok", CanonicalValue(false)},
          });
        }
      }
      const auto result =
          command.kind == CommandKind::kPlanPick
              ? composition.pick_place_planning_owner->PlanPick(
                    *command.pick_planning, planning_stop_token)
              : composition.pick_place_planning_owner->PlanPlace(
                    *command.place_planning, planning_stop_token);
      if (!result.ok()) {
        return CanonicalValue(CanonicalValue::Object{
            {"api_version", CanonicalValue(std::string(kResponseApiVersion))},
            {"error", ErrorValue(result.error())},
            {"ok", CanonicalValue(false)},
        });
      }
      const auto command_name =
          command.kind == CommandKind::kPlanPick ? "plan_pick" : "plan_place";
      const auto kind = command.kind == CommandKind::kPlanPick
                            ? core::PickPlaceExecutionKindDTO::kPick
                            : core::PickPlaceExecutionKindDTO::kPlace;
      return CanonicalValue(CanonicalValue::Object{
          {"api_version", CanonicalValue(std::string(kResponseApiVersion))},
          {"command", CanonicalValue(command_name)},
          {"ok", CanonicalValue(true)},
          {"planning_result",
           PickPlacePlanningResultValue(sequence_id, kind, result.value())},
      });
    }
    if (command.kind == CommandKind::kSubmit) {
      auto origin = composition.target_resolver->ValidateExecutionOrigin(
          *command.operation);
      if (origin.ok())
        origin = VerifyReferenceTargets(*composition.target_resolver,
                                        *command.operation);
      if (!origin.ok()) {
        return CanonicalValue(CanonicalValue::Object{
            {"api_version", CanonicalValue(std::string(kResponseApiVersion))},
            {"error", ErrorValue(origin.error())},
            {"ok", CanonicalValue(false)},
        });
      }
      const auto result =
          composition.active_addressed_kernel()->execute(
              *command.operation);
      if (!result.ok()) {
        return CanonicalValue(CanonicalValue::Object{
            {"api_version", CanonicalValue(std::string(kResponseApiVersion))},
            {"error", ErrorValue(result.error())},
            {"ok", CanonicalValue(false)},
        });
      }
      return CanonicalValue(CanonicalValue::Object{
          {"accepted", AcceptedValue(result.value())},
          {"api_version", CanonicalValue(std::string(kResponseApiVersion))},
          {"command", CanonicalValue("submit")},
          {"ok", CanonicalValue(true)},
      });
    }
    if (command.kind == CommandKind::kGetRuntimeScaleControl) {
      const auto status =
          composition.active_addressed_kernel()
              ->get_runtime_scale_control(
              *command.runtime_scale_control_selector);
      if (!status.ok()) {
        return CanonicalValue(CanonicalValue::Object{
            {"api_version", CanonicalValue(std::string(kResponseApiVersion))},
            {"error", ErrorValue(status.error())},
            {"ok", CanonicalValue(false)},
        });
      }
      return CanonicalValue(CanonicalValue::Object{
          {"api_version", CanonicalValue(std::string(kResponseApiVersion))},
          {"command", CanonicalValue("get_runtime_scale_control")},
          {"ok", CanonicalValue(true)},
          {"runtime_scale_control",
           RuntimeScaleControlStatusValue(status.value())},
      });
    }
    if (command.kind == CommandKind::kSetOperationRuntimeScale) {
      const auto result =
          composition.active_addressed_kernel()->set_runtime_scale(
              *command.operation_runtime_scale);
      if (!result.ok())
        return ErrorResponse(result.error().code, result.error().message);
      const auto &value = result.value();
      return CanonicalValue(CanonicalValue::Object{
          {"api_version", CanonicalValue(std::string(kResponseApiVersion))},
          {"command", CanonicalValue("set_operation_runtime_scale")},
          {"ok", CanonicalValue(true)},
          {"accepted",
           CanonicalValue(CanonicalValue::Object{
               {"request_id", CanonicalValue(value.request_id.value())},
               {"operation_id", CanonicalValue(value.operation_id.value())},
               {"operation_revision", CanonicalValue(static_cast<double>(
                                          value.operation_revision.value()))},
               {"runtime_scale_generation",
                CanonicalValue(static_cast<double>(
                    value.runtime_scale_generation.value()))},
               {"requested_scale", CanonicalValue(value.requested_scale)},
           })},
      });
    }
    if (command.kind == CommandKind::kSetRuntimeScaleControl) {
      const auto result =
          composition.active_addressed_kernel()
              ->set_runtime_scale_control(
              *command.runtime_scale_control);
      if (!result.ok()) {
        return CanonicalValue(CanonicalValue::Object{
            {"api_version", CanonicalValue(std::string(kResponseApiVersion))},
            {"error", ErrorValue(result.error())},
            {"ok", CanonicalValue(false)},
        });
      }
      return CanonicalValue(CanonicalValue::Object{
          {"accepted", RuntimeScaleControlAcceptedValue(result.value())},
          {"api_version", CanonicalValue(std::string(kResponseApiVersion))},
          {"command", CanonicalValue("set_runtime_scale_control")},
          {"ok", CanonicalValue(true)},
      });
    }
    if (command.kind == CommandKind::kApplyOperationRecovery) {
      const auto result =
          composition.active_addressed_kernel()
              ->apply_operation_recovery_action(
                  *command.operation_id, *command.expected_operation_revision,
                  *command.operation_recovery);
      if (!result.ok()) {
        return CanonicalValue(CanonicalValue::Object{
            {"api_version", CanonicalValue(std::string(kResponseApiVersion))},
            {"error", ErrorValue(result.error())},
            {"ok", CanonicalValue(false)},
        });
      }
      return CanonicalValue(CanonicalValue::Object{
          {"api_version", CanonicalValue(std::string(kResponseApiVersion))},
          {"command", CanonicalValue("apply_operation_recovery")},
          {"ok", CanonicalValue(true)},
          {"result", RecoveryActionResultValue(result.value())},
      });
    }
    const auto control_result =
        [&]() -> core::Result<core::OperationControlAcceptedDTO> {
      switch (command.kind) {
      case CommandKind::kReset:
        return composition.active_addressed_kernel()->reset(
            *command.control);
      case CommandKind::kAcknowledgeCompleted:
        return composition.active_addressed_kernel()
            ->acknowledge_completed(
            *command.control);
      case CommandKind::kStatus:
      case CommandKind::kIdentity:
      case CommandKind::kResetRobotPositionCapability:
      case CommandKind::kResetRobotPosition:
      case CommandKind::kTargets:
      case CommandKind::kQueryTarget:
      case CommandKind::kStartMockIoNodeSimulation:
      case CommandKind::kEndMockIoNodeSimulation:
      case CommandKind::kSetMockIoRunPaused:
      case CommandKind::kRegisterIoWatch:
      case CommandKind::kReadIoWatch:
      case CommandKind::kReleaseIoWatch:
      case CommandKind::kSetMockIoInput:
      case CommandKind::kPublishProgramBlackboard:
      case CommandKind::kQueryOperation:
      case CommandKind::kControlState:
      case CommandKind::kObservation:
      case CommandKind::kEvents:
      case CommandKind::kPreviewMotion:
      case CommandKind::kReadManualHandover:
      case CommandKind::kPreviewCapabilityCommand:
      case CommandKind::kRegisterProgramCapture:
      case CommandKind::kSubmit:
      case CommandKind::kRead:
      case CommandKind::kReadStop:
      case CommandKind::kGetOperationRecovery:
      case CommandKind::kApplyOperationRecovery:
      case CommandKind::kStop:
      case CommandKind::kStopProgramRun:
      case CommandKind::kStopProgramSelection:
      case CommandKind::kReadProgramExecutionScope:
      case CommandKind::kReadVolatileMotionTrace:
      case CommandKind::kStartProgramSelection:
      case CommandKind::kReleaseProgramSelection:
      case CommandKind::kCheckProgramNodeMotionAttention:
      case CommandKind::kReleaseProgramRunStopFence:
      case CommandKind::kFullStop:
      case CommandKind::kMalformedStop:
      case CommandKind::kGetStopFault:
      case CommandKind::kRecoverStopFault:
      case CommandKind::kSetRuntimeScaleControl:
      case CommandKind::kSetOperationRuntimeScale:
      case CommandKind::kGetRuntimeScaleControl:
      case CommandKind::kPlanPick:
      case CommandKind::kPlanPlace:
        Fail("execution command dispatch is inconsistent");
      }
      Fail("execution command dispatch is unreachable");
    }();
    if (!control_result.ok()) {
      return CanonicalValue(CanonicalValue::Object{
          {"api_version", CanonicalValue(std::string(kResponseApiVersion))},
          {"error", ErrorValue(control_result.error())},
          {"ok", CanonicalValue(false)},
      });
    }
    return CanonicalValue(CanonicalValue::Object{
        {"accepted", ControlAcceptedValue(control_result.value())},
        {"api_version", CanonicalValue(std::string(kResponseApiVersion))},
        {"command", CanonicalValue(std::string(
                        ControlName(control_result.value().control)))},
        {"ok", CanonicalValue(true)},
    });
  }

[[nodiscard]] std::optional<core::ErrorDTO> ExecutionCommandSurface::LastReconcileError() const {
    std::lock_guard lock(reconcile_error_mutex_);
    return last_reconcile_error_;
  }

[[nodiscard]] core::Result<void>
  ExecutionCommandSurface::ReconcileDirectControlOperations(Composition &composition) const {
    // One exact Kernel claim identifies manual work, independent of caller
    // labels. Reuse this owner lane; browsers never trigger terminal release.
    return composition.active_addressed_kernel()
        ->reconcile_manual_operation_admission();
  }

void ExecutionCommandSurface::RunReconciliationLane(std::stop_token stop_token) noexcept {
    try {
      std::optional<core::TimePoint> last_recovery_observation;
      std::optional<core::ErrorDTO> recovery_error;
      while (!stop_token.stop_requested()) {
#if defined(MANYFORGE_ENABLE_FOCUSED_PICK_PLACE_PLANNING_FIXTURE)
        const char *marker =
            std::getenv("MANYFORGE_RECONCILIATION_TEST_MARKER");
        const char *gate = std::getenv("MANYFORGE_RECONCILIATION_TEST_GATE");
        if (marker != nullptr && marker[0] != '\0' && gate != nullptr &&
            gate[0] != '\0') {
          const int descriptor =
              ::open(marker, O_WRONLY | O_CREAT | O_CLOEXEC, 0600);
          if (descriptor >= 0) {
            static_cast<void>(::close(descriptor));
          }
          while (!stop_token.stop_requested() && ::access(gate, F_OK) != 0) {
            pollfd interruptible_wait{-1, 0, 0};
            static_cast<void>(
                ::poll(&interruptible_wait, 0, kReconcilePeriodMilliseconds));
          }
        }
        const char *injected_failure =
            std::getenv("MANYFORGE_RECONCILIATION_TEST_THROW");
        if (!stop_token.stop_requested() && injected_failure != nullptr &&
            injected_failure[0] != '\0') {
          throw std::runtime_error(injected_failure);
        }
#endif
        {
          // This shared lifetime lock prevents Manager reconfiguration from
          // retiring a composition while its subordinate owners are in use.
          std::shared_lock lock(composition_access_.mutex);
          if (composition_access_.current != nullptr) {
            auto &composition = *composition_access_.current;
#if defined(MANYFORGE_ROS2_CONTROL_EXECUTION_HOST)
            const auto now = composition.runtime ? composition.runtime->clock->now()
                : composition.cell_runtime->clock()->now();
#else
            const auto cell_clock = composition.cell_runtime != nullptr
                                        ? composition.cell_runtime->clock()
                                    : composition.runtime != nullptr
                                        ? composition.runtime->clock
                                        : nullptr;
            Require(cell_clock != nullptr,
                    "native reconciliation has no stable cell clock");
            const auto now = cell_clock->now();
#endif
            if (!last_recovery_observation ||
                now < *last_recovery_observation ||
                now - *last_recovery_observation >=
                    kRecoveryObservationPeriodNs) {
              last_recovery_observation = now;
#if defined(MANYFORGE_ROS2_CONTROL_EXECUTION_HOST)
              const bool has_route_reconciler = composition.runtime
                  ? static_cast<bool>(composition.runtime->reconcile_controller_routes)
                  : static_cast<bool>(composition.cclc_cell_route_reconciler);
              const auto routes = has_route_reconciler
                  ? (composition.runtime ? composition.runtime->reconcile_controller_routes()
                                         : composition.cclc_cell_route_reconciler())
                  : core::Result<void>::Ok();
#else
              const bool has_route_reconciler =
                  static_cast<bool>(composition.cclc_cell_route_reconciler) ||
                  (composition.runtime != nullptr &&
                   static_cast<bool>(
                       composition.runtime->reconcile_controller_routes));
              const auto routes =
                  composition.cclc_cell_route_reconciler
                      ? composition.cclc_cell_route_reconciler()
                      : composition.runtime != nullptr &&
                                composition.runtime->reconcile_controller_routes
                            ? composition.runtime->reconcile_controller_routes()
                            : core::Result<void>::Ok();
#endif
              if (has_route_reconciler && routes.ok()) {
                composition.route_recovery_pending.store(false,
                                                         std::memory_order_release);
              }
              const auto assessed =
                  composition.active_addressed_kernel()
                      ->refresh_stop_fault_recovery_assessment();
              recovery_error = !routes.ok() ? std::optional(routes.error())
                               : !assessed.ok()
                                   ? std::optional(assessed.error())
                                   : std::nullopt;
            }
            auto error = [&]() -> std::optional<core::ErrorDTO> {
              const auto reconciled = composition.active_addressed_kernel()
                                          ->reconcile_subordinate_state();
              if (!reconciled.ok()) {
                return reconciled.error();
              }
              const auto manual_settlement =
                  ReconcileDirectControlOperations(composition);
              if (!manual_settlement.ok())
                return manual_settlement.error();
              return std::nullopt;
            }();
            std::lock_guard error_lock(reconcile_error_mutex_);
            last_reconcile_error_ = error ? std::move(error) : recovery_error;
          }
        }
        pollfd interruptible_wait{-1, 0, 0};
        int waited = -1;
        do {
          waited = ::poll(&interruptible_wait, 0, kReconcilePeriodMilliseconds);
        } while (waited < 0 && errno == EINTR && !stop_token.stop_requested());
        if (waited < 0 && !stop_token.stop_requested()) {
          return;
        }
      }
    } catch (...) {
      const auto failure = std::current_exception();
      {
        std::lock_guard lock(reconcile_error_mutex_);
        reconciliation_lane_failure_ = failure;
      }
      reconciliation_lane_failed_.store(true, std::memory_order_release);
      WakeLanes();
    }
  }

/// Socket lanes only wait for clients; they need no periodic wakeup.
  /// Every reason to leave a lane (its stop request, a failed lane, a process
  /// stop signal) makes one of these descriptors readable.
  [[nodiscard]] int ExecutionCommandSurface::WaitForSocket(const int socket, short &revents) const {
    std::array<pollfd, 3U> descriptors{{
        {socket, POLLIN, 0},
        {lane_wake_.get(), POLLIN, 0},
        // A negative descriptor is ignored by poll().
        {g_stop_signal_wake_fd.load(std::memory_order_relaxed), POLLIN, 0},
    }};
    const int ready = ::poll(descriptors.data(), descriptors.size(), -1);
    revents = descriptors[0].revents;
    return ready;
  }

/// Ends every socket-lane wait. The wake stays readable: it is raised only
  /// when this surface is stopping.
  void ExecutionCommandSurface::WakeLanes() noexcept {
    const std::uint64_t one = 1U;
    const ssize_t written = ::write(lane_wake_.get(), &one, sizeof(one));
    // The eventfd is nonblocking and stays readable after the first wake;
    // subsequent best-effort writes do not change the stopping decision.
    static_cast<void>(written);
  }

[[nodiscard]] bool ExecutionCommandSurface::LanesWoken() const noexcept {
    pollfd wake{lane_wake_.get(), POLLIN, 0};
    return ::poll(&wake, 1, 0) > 0 && (wake.revents & POLLIN) != 0;
  }

void ExecutionCommandSurface::RunControlLane(std::stop_token stop_token) noexcept {
    const std::stop_callback wake_on_stop(stop_token,
                                          [this] { WakeLanes(); });
    while (!stop_token.stop_requested() && g_stop_requested == 0 &&
           !LanesWoken()) {
      short revents = 0;
      int ready = -1;
      do {
        ready = WaitForSocket(control_socket_.get(), revents);
      } while (ready < 0 && errno == EINTR && !stop_token.stop_requested());
      if (stop_token.stop_requested()) {
        return;
      }
      if (ready < 0 || (revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
        control_socket_failed_.store(true, std::memory_order_release);
        WakeLanes();
        return;
      }
      if ((revents & POLLIN) != 0) {
        HandleOneClient(control_socket_, CommandLane::kStop);
      }
    }
  }

void ExecutionCommandSurface::RunObservationLane(std::stop_token stop_token) noexcept {
    const std::stop_callback wake_on_stop(stop_token,
                                          [this] { WakeLanes(); });
    while (!stop_token.stop_requested() && g_stop_requested == 0 &&
           !LanesWoken()) {
      short revents = 0;
      int ready = -1;
      do {
        ready = WaitForSocket(observation_socket_.get(), revents);
      } while (ready < 0 && errno == EINTR && !stop_token.stop_requested());
      if (stop_token.stop_requested() || ready < 0 ||
          (revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
        return;
      }
      if ((revents & POLLIN) != 0) {
        HandleOneClient(observation_socket_, CommandLane::kObservation,
                        stop_token);
      }
    }
  }

void ExecutionCommandSurface::HandleOneClient(const CommandSocket &socket, const CommandLane lane,
                       const std::stop_token stop_token) noexcept {
    const bool ordinary = lane == CommandLane::kOrdinary;
    try {
      FileDescriptor client(::accept4(socket.get(), nullptr, nullptr,
                                      SOCK_CLOEXEC | SOCK_NONBLOCK));
      if (!client) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
          return;
        }
        FailErrno("accept execution command client");
      }
      ucred peer{};
      socklen_t peer_size = sizeof(peer);
      Require(::getsockopt(client.get(), SOL_SOCKET, SO_PEERCRED, &peer,
                           &peer_size) == 0 &&
                  peer_size == sizeof(peer) && peer.uid == ::geteuid(),
              "execution command client is not the same user");
      bool first_packet = true;
      while (!stop_token.stop_requested()) {
        pollfd inbound{client.get(), POLLIN, 0};
        int packet_ready = -1;
        do {
          packet_ready =
              ::poll(&inbound, 1,
                     first_packet ? kClientPacketDeadlineMilliseconds : 1'000);
        } while (packet_ready < 0 && errno == EINTR);
        if (lane == CommandLane::kObservation && !first_packet &&
            packet_ready == 0) {
          continue;
        }
        Require(packet_ready > 0 && (inbound.revents & POLLIN) != 0,
                "execution command client did not send a packet in time");
        first_packet = false;
        std::array<char, kMaximumPacketBytes> buffer{};
        iovec vector{buffer.data(), buffer.size()};
        msghdr message{};
        message.msg_iov = &vector;
        message.msg_iovlen = 1U;
        ssize_t count = -1;
        do {
          count = ::recvmsg(client.get(), &message, 0);
        } while (count < 0 && errno == EINTR);
        CanonicalValue response;
        std::optional<std::string> cell_admission_command;
        std::shared_ptr<core::IIoSignalService> actual_retired_service;
        std::optional<std::pair<DeferredLane *, Command>> deferred;
        if (count <= 0 || (message.msg_flags & MSG_TRUNC) != 0) {
          response =
              ErrorResponse("execution_command_invalid",
                            "execution command packet is empty or oversized");
        } else {
          try {
            const auto bytes = std::string_view(
                buffer.data(), static_cast<std::size_t>(count));
            for (const auto candidate :
                 {"cell_admission_get", "cell_admission_update_readiness",
                  "cell_admission_acquire", "cell_admission_release",
                  "cell_admission_reconcile",
                  "cell_admission_recovery_receipt",
                  "cell_admission_publish_operator_alarm_attention"}) {
              if (bytes.find(std::string("\"command\":\"") + candidate +
                             "\"") != std::string_view::npos) {
                cell_admission_command = candidate;
                break;
              }
            }
            const auto cell_admission =
                detail::TryDecodeCellAdmissionCommand(bytes);
            if (cell_admission.has_value()) {
              const bool attention =
                  cell_admission->command ==
                  component_manager::detail::
                      kPublishOperatorAlarmAttentionCommand;
              if (lane == CommandLane::kObservation) {
                response = detail::CellAdmissionResponseValue(
                    cell_admission->command,
                    core::ErrorDTO{
                        "execution_observation_command_unsupported",
                        "the independent observation socket accepts only "
                        "observation",
                        {}});
              } else if (lane == CommandLane::kStop && !attention) {
                response = detail::CellAdmissionResponseValue(
                    cell_admission->command,
                    core::ErrorDTO{
                        "execution_control_command_unsupported",
                        "the independent Stop control socket accepts only "
                        "Stop, Stop status and operator-alarm attention",
                        {}});
              } else if (lane == CommandLane::kOrdinary && attention) {
                response = detail::CellAdmissionResponseValue(
                    cell_admission->command,
                    core::ErrorDTO{
                        "execution_ordinary_command_unsupported",
                        "operator-alarm attention is published on the "
                        "independent Stop control socket",
                        {}});
              } else {
                // Cell admission is owned by the stable cell Kernel and never
                // reads the composition, so an Apply holding the composition
                // exclusively does not delay it.
                if (PreemptsPlanning(*cell_admission)) {
                  PreemptPlanning();
                }
                auto result =
                    component_manager::detail::DispatchKernelCellAdmissionRpc(
                        StableKernel(), *cell_admission,
                        &actual_retired_service);
                response = detail::CellAdmissionResponseValue(
                    cell_admission->command, result);
              }
            } else {
              const auto command = DecodeCommand(bytes);
              if (lane != CommandLane::kObservation &&
                  PreemptsPlanning(command.kind)) {
                PreemptPlanning();
              }
              if (lane == CommandLane::kStop) {
                // Stop and its in-memory progression reads reach the stable
                // Kernel without the composition lock. It owns exact scope
                // resolution, journal fences and unavailable evidence, also
                // while no physical product is composed; the host cannot
                // infer rest from the absence of one.
                auto &owner = StableKernel();
                response = command.kind == CommandKind::kReadStop
                               ? ReadStop(command, owner)
                           : command.kind == CommandKind::kGetStopFault
                               ? ReadStopFault(owner)
                               : ExecuteStop(command, owner);
              } else if (lane == CommandLane::kObservation &&
                         command.kind != CommandKind::kObservation &&
                         command.kind != CommandKind::kReadVolatileMotionTrace) {
                response = ErrorResponse(
                    "execution_observation_command_unsupported",
                    "the independent observation socket accepts only "
                    "observation");
              } else if (IsStopLaneRead(command.kind)) {
                response = ErrorResponse(
                    "execution_ordinary_command_unsupported",
                    "Stop status and Stop-fault reads are served by the "
                    "independent Stop control socket");
              } else if (command.kind == CommandKind::kReadVolatileMotionTrace) {
#if defined(MANYFORGE_ROS2_CONTROL_EXECUTION_HOST)
                const auto trace = composition_access_.motion_trace;
                auto snapshot = trace
                    ? trace->Snapshot(owner_epoch_, command.trace_allowance_bytes)
                    : core::Result<CanonicalValue>::Err({"volatile_motion_trace_unavailable",
                        "this execution host has no volatile trace collector", {}});
                response = snapshot.ok()
                    ? CanonicalValue(CanonicalValue::Object{
                        {"api_version", CanonicalValue(std::string(kResponseApiVersion))},
                        {"command", CanonicalValue("read_volatile_motion_trace")},
                        {"ok", CanonicalValue(true)}, {"trace", std::move(snapshot.value())}})
                    : ErrorResponse(snapshot.error().code, snapshot.error().message,
                                    snapshot.error().details);
#else
                response = ErrorResponse("volatile_motion_trace_unavailable",
                    "this execution host has no ROS trajectory collector");
#endif
              } else if (command.kind == CommandKind::kEvents) {
                response = EventsResponse(command);
              } else if (command.kind == CommandKind::kRecoverStopFault &&
                         command.global_io_rule_recovery) {
                // Same operator cell-recovery command, exact global row
                // selector. This owner exists even without a physical product.
                response = RecoverGlobalIoRule(*command.global_io_rule_recovery,
                                               StableKernel());
              } else if (command.kind ==
                         CommandKind::kPublishProgramBlackboard) {
                // ACK follows synchronous exact-Run source admission, never
                // mere socket receipt or an enqueue awaiting later work.
                response = ProgramBlackboardResponse(
                    *command.blackboard_publication, StableKernel());
              } else if (command.kind == CommandKind::kRegisterIoWatch ||
                         command.kind == CommandKind::kReadIoWatch ||
                         command.kind == CommandKind::kReleaseIoWatch) {
                // Zero-claim watches use the stable captured authority, also
                // after Run teardown has removed the physical composition.
                response = IoWatchResponse(command, StableKernel());
              } else {
                std::shared_lock composition_lock(composition_access_.mutex);
                if (composition_access_.product() == nullptr) {
                  response = OwnerNotReadyResponse(command);
                } else if (composition_access_.current == nullptr) {
                  if (ordinary && IsSubmissionOrdered(command.kind))
                    deferred.emplace(&submission_ordered_lane_, command);
                  else if (ordinary && ReadsManagerScene(command.kind))
                    deferred.emplace(&scene_read_lane_, command);
                  else
                    response = ExecuteLogicalProduct(command, *composition_access_.product());
                } else if (lane == CommandLane::kObservation &&
                           command.observation_cursor_supplied) {
                  composition_lock.unlock();
                  response = ObservationResponseAfter(command, client.get(),
                                                      stop_token);
                } else if (ordinary && IsSubmissionOrdered(command.kind)) {
                  deferred.emplace(&submission_ordered_lane_, command);
                } else if (ordinary && ReadsManagerScene(command.kind)) {
                  deferred.emplace(&scene_read_lane_, command);
                } else {
                  response = Execute(
                      command, *composition_access_.current,
                      composition_access_.current->projection_revision,
                      std::stop_token{});
                  if (command.kind == CommandKind::kReset) {
                    const auto *accepted = response.find_object_member("ok");
                    if (accepted != nullptr && accepted->is_bool() &&
                        accepted->as_bool()) {
                      PreemptPlanning();
                    }
                  }
                }
              }
            }
          } catch (const std::exception &error) {
            response =
                cell_admission_command.has_value()
                    ? detail::CellAdmissionResponseValue(
                          *cell_admission_command,
                          core::ErrorDTO{
                              "execution_command_invalid", error.what(), {}})
                    : ErrorResponse("execution_command_invalid", error.what());
          }
        }
        // Core and composition scopes have unwound. Only a real source
        // teardown requests work; unchanged readiness heartbeats stay inert.
        if (auto runtime = std::dynamic_pointer_cast<HostIoSignalRuntime>(actual_retired_service))
          runtime->RequestRuleEvidenceProcessing();
        if (deferred.has_value()) {
          // The composition lock is released: the deferred lane takes its own
          // while it executes.
          auto &[deferred_lane, deferred_command] = *deferred;
          if (EnqueueDeferred(*deferred_lane, client,
                              std::move(deferred_command))) {
            return;
          }
          response = ErrorResponse(
              std::string(deferred_lane->busy_code),
              "the execution owner already holds its bounded number of "
              "pending requests on this lane");
        }
        SendResponse(client.get(), response, cell_admission_command);
        if (lane != CommandLane::kObservation) {
          return;
        }
      }
    } catch (...) {
      // A malformed/disappearing private client cannot retire the sole owner.
    }
  }

void ExecutionCommandSurface::SendResponse(
      const int client, const CanonicalValue &response,
      const std::optional<std::string> &cell_admission_command) const {
    auto bytes = content_identity::canonical_json(response);
    if (bytes.empty() || bytes.size() > kMaximumResponseBytes) {
      const core::ErrorDTO error{
          "execution_command_response_oversized",
          "execution owner result exceeds the bounded private transport",
          {}};
      bytes = content_identity::canonical_json(
          cell_admission_command.has_value()
              ? detail::CellAdmissionResponseValue(*cell_admission_command,
                                                   error)
              : ErrorResponse(error.code, error.message));
    }
    pollfd outbound{client, POLLOUT, 0};
    int response_ready = -1;
    do {
      response_ready = ::poll(&outbound, 1, kClientPacketDeadlineMilliseconds);
    } while (response_ready < 0 && errno == EINTR);
    Require(response_ready > 0 && (outbound.revents & POLLOUT) != 0,
            "execution command client did not accept a response in time");
    ssize_t sent = -1;
    do {
      sent = ::send(client, bytes.data(), bytes.size(), MSG_NOSIGNAL);
    } while (sent < 0 && errno == EINTR);
    Require(sent == static_cast<ssize_t>(bytes.size()),
            "execution command response was not sent atomically");
  }

/// Lane and ordering contract of the execution sockets.
  ///
  /// Every ordinary and control connection carries one request and its
  /// response, so a client that needs A before B already waits for A's
  /// response; requests on different connections or sockets are ordered by
  /// nothing but the owner fences below, never by arrival.
  ///
  /// - Control socket: Stop, Program Stop, Program-selection Stop, full and
  ///   malformed Stop, plus the in-memory reads Stop progression needs
  ///   (Stop status, Stop fault) and the operator-alarm attention heartbeat.
  ///   It reaches only the stable Kernel, without the composition lock, so
  ///   neither planning nor an Apply delays it. Each request is a bounded
  ///   copy under the Kernel's state lock.
  /// - Ordinary socket: operation controls (Reset,
  ///   acknowledgement), events, reads, Program selection and capture, cell
  ///   admission and recovery. It never waits for planning, submission
  ///   ordering or a Manager round trip; those go to deferred lanes.
  /// - Submission-ordered lane: requests whose Kernel entry takes submission
  ///   ordering (IsSubmissionOrdered). One worker, FIFO in accept order, one
  ///   in flight, as the single ordinary lane executed them before.
  /// - Scene-read lane: projections that first read the live Scene revision
  ///   from Manager (ReadsManagerScene). One worker, FIFO.
  ///
  /// Cross-lane fences: Reset, recovery and Stop-fence release name the
  /// exact operation revision, Stop id and generation, or Stop-fault latch
  /// they act on, and their clients issue them only after reading that state
  /// on the control socket. A stale name is refused (the Program owner also
  /// refuses a cycle Reset while its Stop is not terminal); a Kernel Reset of
  /// a participant still stopping hard-stops it first, so no ordering of the
  /// two sockets lets Reset restart motion or release a newer Stop. Alarm
  /// attention is fenced by owner epoch, incarnation and monotonic generation
  /// and evaluated at admission only by age, so admission on another lane
  /// observes either the previous or the new publication, both valid. Event
  /// pages are journal-sequence ordered.
  [[nodiscard]] core::ResourceAddressedExecutionKernel &ExecutionCommandSurface::StableKernel() const {
    auto *const owner = composition_access_.addressed_kernel.load();
    Require(owner != nullptr,
            "stable addressed execution Kernel is not installed");
    return *owner;
  }

[[nodiscard]] CanonicalValue
  ExecutionCommandSurface::OwnerNotReadyResponse(const Command &command) const {
    if (command.kind == CommandKind::kIdentity) {
      return ExecutionOwnerNotReadyIdentityResponse(StableKernel(),
                                                    owner_epoch_);
    }
    if (command.kind == CommandKind::kResetRobotPosition) {
      return ErrorResponse("execution_owner_not_ready",
                           "the managed execution owner has no active physical product",
                           {{"owner_epoch", owner_epoch_.value()},
                            {"effect_disposition", "known_no_effect"}});
    }
    return ErrorResponse("execution_owner_not_ready",
                         "the managed execution owner has no active physical "
                         "product",
                         {{"owner_epoch", owner_epoch_.value()}});
  }

[[nodiscard]] bool ExecutionCommandSurface::EnqueueDeferred(DeferredLane &lane, FileDescriptor &client,
                                     Command command) {
    {
      std::lock_guard lock(lane.mutex);
      const auto pending =
          lane.queue.size() + (lane.in_flight.has_value() ? 1U : 0U);
      if (pending >= kMaximumPendingDeferredCommands) {
        return false;
      }
      lane.queue.push_back(PendingDeferredCommand{
          std::move(client), std::move(command), std::stop_source{}});
    }
    lane.ready.notify_one();
    return true;
  }

/// Cancels every advisory planning query already accepted, executing or
  /// queued; a queued one then reaches the Kernel already cancelled. Only
  /// planning consults the token: a queued submit or read is unaffected.
  /// Called before a Stop or a component-lifecycle cell-admission claim is
  /// dispatched and after a Reset is accepted; none waits for the cancelled
  /// planning to return. (The Kernel's own
  /// Reset of a participant additionally preempts that participant's preview
  /// before taking its lifecycle lock.)
  void ExecutionCommandSurface::PreemptPlanning() noexcept {
    std::lock_guard lock(submission_ordered_lane_.mutex);
    for (auto &pending : submission_ordered_lane_.queue) {
      pending.cancellation.request_stop();
    }
    if (submission_ordered_lane_.in_flight.has_value()) {
      submission_ordered_lane_.in_flight->request_stop();
    }
  }

[[nodiscard]] CanonicalValue
  ExecutionCommandSurface::DeferredResponse(const Command &command, const std::stop_token cancellation,
                   const std::stop_token lane_stop_token) {
    std::shared_lock composition_lock(composition_access_.mutex);
    if (composition_access_.product() == nullptr)
      return OwnerNotReadyResponse(command);
    if (composition_access_.current == nullptr)
      return ExecuteLogicalProduct(command, *composition_access_.product());
    auto projection = composition_access_.current->projection_revision;
    const auto scene = composition_access_.current->scene_authority_proxy;
    if (ReadsManagerScene(command.kind) && scene != nullptr) {
      const auto generation =
          composition_access_.current->publication_generation;
      // Manager may be waiting to replace this composition. Never retain
      // its lock across a request back to Manager. The shared proxy
      // outlives the unlocked read; only the exact captured publication may
      // consume its result.
      composition_lock.unlock();
      projection = scene->CurrentProjectionRevision(lane_stop_token);
      composition_lock.lock();
      Require(composition_access_.current != nullptr &&
                  composition_access_.current->publication_generation ==
                      generation &&
                  composition_access_.current->scene_authority_proxy == scene,
              "execution identity changed during Scene read");
    }
    return Execute(command, *composition_access_.current, projection,
                   cancellation);
  }

void ExecutionCommandSurface::RunDeferredLane(DeferredLane &lane,
                       const std::stop_token stop_token) noexcept {
    for (;;) {
      std::optional<PendingDeferredCommand> pending;
      {
        std::unique_lock lock(lane.mutex);
        if (!lane.ready.wait(lock, stop_token,
                             [&lane] { return !lane.queue.empty(); })) {
          // Closing the queued clients is their answer: the owner is leaving.
          lane.queue.clear();
          return;
        }
        pending.emplace(std::move(lane.queue.front()));
        lane.queue.pop_front();
        lane.in_flight = pending->cancellation;
      }
      const std::stop_callback cancel_on_shutdown(
          stop_token, [source = pending->cancellation]() mutable {
            source.request_stop();
          });
      try {
        CanonicalValue response;
        try {
          response = DeferredResponse(pending->command,
                                      pending->cancellation.get_token(),
                                      stop_token);
        } catch (const std::exception &error) {
          response = ErrorResponse("execution_command_invalid", error.what());
        }
        {
          std::lock_guard lock(lane.mutex);
          lane.in_flight.reset();
        }
        SendResponse(pending->client.get(), response, std::nullopt);
      } catch (...) {
        // A disappearing private client cannot retire the sole owner.
        std::lock_guard lock(lane.mutex);
        lane.in_flight.reset();
      }
    }
  }

void RunManagedExecutionCommandSurface(
    ManagedExecutionSurfaceControl &control,
    const std::stop_token stop_token) noexcept {
  try {
    Require(control.composition_access != nullptr &&
                control.manager_liveness_admission != nullptr &&
                !control.owner_epoch.empty(),
            "managed execution command surface configuration is incomplete");
    ExecutionCommandSurface surface(
        control.run_root, *control.composition_access,
        *control.manager_liveness_admission, control.owner_epoch);
    // Unpublish the surface before it is destroyed, also when Run throws.
    struct Unpublish final {
      ManagedExecutionSurfaceControl &control;
      ~Unpublish() {
        std::lock_guard lock(control.state_mutex);
        control.surface = nullptr;
      }
    } const unpublish{control};
    {
      std::lock_guard lock(control.state_mutex);
      control.surface = &surface;
      control.ready = true;
    }
    control.state_changed.notify_all();
    static_cast<void>(surface.Run(stop_token));
  } catch (...) {
    {
      std::lock_guard lock(control.state_mutex);
      control.failure = std::current_exception();
    }
    control.state_changed.notify_all();
  }
}

void PreemptManagedExecutionSurfacePlanning(
    ManagedExecutionSurfaceControl &control) noexcept {
  std::lock_guard lock(control.state_mutex);
  if (control.surface != nullptr) {
    control.surface->PreemptPlanning();
  }
}


} // namespace manyforge::roboplan_backend::host_detail
