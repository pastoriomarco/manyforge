#include <manyforge_roboplan/retained_native_gripper_route_publication_owner.hpp>

#include <stdexcept>
#include <string>
#include <utility>

namespace manyforge::roboplan_backend {
namespace {

template <typename Value>
[[nodiscard]] core::Result<Value> route_publication_error(
    std::string reason, std::string message,
    const core::ErrorCode code = core::ErrorCode::kInvariantViolation) {
  return core::Result<Value>::Err(core::ErrorDTO{
      std::string(core::to_string(code)), std::move(message),
      {{"reason", std::move(reason)}}});
}

}  // namespace

RetainedNativeGripperRoutePublicationOwner::
    RetainedNativeGripperRoutePublicationOwner(
        std::shared_ptr<RetainedNativeGripperRuntime> gripper,
        core::IClock& clock, core::ResourceAddressedExecutionKernel& kernel,
        CellKernelCandidateBuilder cell_candidate_builder)
    : gripper_(std::move(gripper)), clock_(clock), kernel_(kernel),
      cell_candidate_builder_(std::move(cell_candidate_builder)) {
  if (gripper_ == nullptr || gripper_->participant_engine() == nullptr) {
    throw std::invalid_argument(
        "native gripper route publication requires one retained gripper owner");
  }
  const auto binding = gripper_->binding();
  const auto registration = gripper_->registration();
  if (!binding.value().control_path.has_value() ||
      binding.value().resources.empty() || registration.engine == nullptr ||
      registration.engine != gripper_->participant_engine() ||
      registration.binding.value() != binding.value()) {
    throw std::invalid_argument(
        "native gripper route publication owner has an invalid exact participant");
  }
}

core::Result<std::vector<core::RuntimeParticipantBinding>>
RetainedNativeGripperRoutePublicationOwner::ReadCurrentPhysicalParticipants(
    const core::ActiveControlConfigurationDTO& configuration) const {
  if (!configuration.locally_valid() ||
      configuration.state !=
          core::ActiveControlConfigurationStateDTO::kReconciled) {
    return route_publication_error<
        std::vector<core::RuntimeParticipantBinding>>(
        "route_publication_configuration_invalid",
        "native gripper physical read requires a valid reconciled configuration",
        core::ErrorCode::kInvalidRequest);
  }
  if (configuration.binding_routes.size() != 1U) {
    return route_publication_error<
        std::vector<core::RuntimeParticipantBinding>>(
        "route_publication_gripper_scope_invalid",
        "standalone gripper route publication requires exactly one selected binding",
        core::ErrorCode::kInvalidRequest);
  }

  std::lock_guard lock(mutex_);
  const auto binding = gripper_->binding();
  const auto& value = binding.value();
  if (!value.control_path.has_value()) {
    return route_publication_error<
        std::vector<core::RuntimeParticipantBinding>>(
        "route_publication_gripper_binding_missing",
        "retained gripper no longer has a current control path",
        core::ErrorCode::kMissingReference);
  }
  const auto& path = *value.control_path;
  const auto& selected = configuration.binding_routes.front();
  if (path.control_domain_id != configuration.control_domain_id ||
      path.control_binding_id != selected.control_binding_id ||
      path.control_binding_revision != selected.control_binding_revision) {
    return route_publication_error<
        std::vector<core::RuntimeParticipantBinding>>(
        "route_publication_physical_binding_missing",
        "retained gripper does not own the selected physical binding",
        core::ErrorCode::kMissingReference);
  }
  return core::Result<std::vector<core::RuntimeParticipantBinding>>::Ok(
      {binding});
}

core::Result<core::RuntimeRouteConfigurationReadbackDTO>
RetainedNativeGripperRoutePublicationOwner::
    PublishAndReadBackRouteConfiguration(
        const core::ActiveControlConfigurationDTO& configuration,
        std::vector<deployment::VerifiedRouteReadinessParticipant> participants) {
  if (!configuration.locally_valid()) {
    return route_publication_error<core::RuntimeRouteConfigurationReadbackDTO>(
        "route_publication_configuration_invalid",
        "native gripper route publication has an invalid configuration",
        core::ErrorCode::kInvalidRequest);
  }
  if (configuration.binding_routes.size() != 1U) {
    return route_publication_error<core::RuntimeRouteConfigurationReadbackDTO>(
        "route_publication_gripper_scope_invalid",
        "standalone gripper route publication requires exactly one selected binding",
        core::ErrorCode::kInvalidRequest);
  }

  std::lock_guard lock(mutex_);
  const auto old_binding = gripper_->binding();
  const auto old_readiness = gripper_->stop_readiness();
  const auto old_channel = gripper_->state_channel();
  if (!old_binding.value().control_path.has_value() ||
      old_binding.value().control_path->control_domain_id !=
          configuration.control_domain_id) {
    return route_publication_error<core::RuntimeRouteConfigurationReadbackDTO>(
        "route_publication_gripper_domain_mismatch",
        "gripper route publication does not match its retained domain",
        core::ErrorCode::kInvalidRequest);
  }

  if (configuration.state ==
      core::ActiveControlConfigurationStateDTO::kExternalOrUnknownUnreconciled) {
    if (!participants.empty()) {
      return route_publication_error<core::RuntimeRouteConfigurationReadbackDTO>(
          "route_publication_gripper_unreconciled_participants_present",
          "unreconciled gripper route publication cannot carry readiness participants",
          core::ErrorCode::kInvalidRequest);
    }
    const auto published =
        kernel_.publish_runtime_route_configuration(configuration);
    if (!published.ok()) {
      return published;
    }
    const auto readback = kernel_.runtime_route_configuration_readback(
        configuration.control_domain_id,
        configuration.active_control_configuration_id,
        configuration.active_control_configuration_generation);
    if (!readback.ok()) {
      return readback;
    }
    if (readback.value() != published.value() ||
        readback.value().active_configuration != configuration ||
        !readback.value().readiness_rows.empty() ||
        readback.value().operation_admission_open) {
      return route_publication_error<core::RuntimeRouteConfigurationReadbackDTO>(
          "route_publication_gripper_unreconciled_readback_mismatch",
          "Kernel gripper route readback differs from the closed unreconciled publication");
    }
    return readback;
  }
  if (configuration.state !=
      core::ActiveControlConfigurationStateDTO::kReconciled) {
    return route_publication_error<core::RuntimeRouteConfigurationReadbackDTO>(
        "route_publication_configuration_invalid",
        "native gripper route publication has an unknown state",
        core::ErrorCode::kInvalidRequest);
  }
  if (!cell_candidate_builder_) {
    return route_publication_error<core::RuntimeRouteConfigurationReadbackDTO>(
        "route_publication_gripper_cell_builder_missing",
        "reconciled gripper publication requires the stable cell owner to build the complete Kernel registration set",
        core::ErrorCode::kInvalidRequest);
  }
  if (participants.size() != 1U) {
    return route_publication_error<core::RuntimeRouteConfigurationReadbackDTO>(
        "route_publication_participant_set_invalid",
        "single-gripper reconciled publication requires exactly one readiness participant",
        core::ErrorCode::kInvalidRequest);
  }

  const auto& verified = participants.front();
  const auto& candidate_binding = verified.participant_binding;
  const auto& candidate_value = candidate_binding.value();
  if (!candidate_value.control_path.has_value() ||
      !verified.readiness_row.locally_valid()) {
    return route_publication_error<core::RuntimeRouteConfigurationReadbackDTO>(
        "route_publication_participant_referent_mismatch",
        "gripper readiness participant has no exact sealed route identity",
        core::ErrorCode::kInvalidRequest);
  }
  const auto& candidate_path = *candidate_value.control_path;
  const auto& selected = configuration.binding_routes.front();
  const auto& readiness_value = verified.stop_readiness.value();
  if (candidate_path.control_domain_id != configuration.control_domain_id ||
      candidate_path.control_domain_revision !=
          configuration.control_domain_revision ||
      candidate_path.control_binding_id != selected.control_binding_id ||
      candidate_path.control_binding_revision !=
          selected.control_binding_revision ||
      candidate_path.control_route_id != selected.control_route_id ||
      candidate_path.activation_set_id != configuration.activation_set_id ||
      candidate_path.active_control_configuration_id !=
          configuration.active_control_configuration_id ||
      candidate_path.active_control_configuration_generation !=
          configuration.active_control_configuration_generation ||
      candidate_path.controller_topology_id !=
          configuration.controller_topology_id ||
      candidate_path.topology_revision != configuration.topology_revision ||
      candidate_path.provider_runtime_unit_id !=
          configuration.source_runtime_unit_id ||
      verified.readiness_row.controllable_component_id !=
          candidate_value.controllable_component_id ||
      verified.readiness_row.component_lifecycle_generation !=
          readiness_value.component_lifecycle_generation ||
      verified.readiness_row.control_binding_id !=
          candidate_path.control_binding_id ||
      verified.readiness_row.control_binding_revision !=
          candidate_path.control_binding_revision ||
      verified.readiness_row.resolved_stop_base_plan_id !=
          readiness_value.resolved_stop_base_plan.resolved_stop_base_plan_id ||
      readiness_value.controllable_component_id !=
          candidate_value.controllable_component_id ||
      readiness_value.expected_controller_domain.control_domain_id !=
          configuration.control_domain_id ||
      readiness_value.expected_controller_domain.control_domain_revision !=
          configuration.control_domain_revision) {
    return route_publication_error<core::RuntimeRouteConfigurationReadbackDTO>(
        "route_publication_participant_referent_mismatch",
        "gripper participant, readiness row, and reconciled configuration differ",
        core::ErrorCode::kInvalidRequest);
  }
  const auto expected_domain = core::verify_controller_lifecycle_expected_domain(
      readiness_value.expected_controller_domain);
  if (!expected_domain.ok()) {
    return core::Result<core::RuntimeRouteConfigurationReadbackDTO>::Err(
        expected_domain.error());
  }
  const auto readiness_check = core::verify_runtime_participant_stop_readiness(
      readiness_value, candidate_binding, expected_domain.value());
  if (!readiness_check.ok()) {
    return core::Result<core::RuntimeRouteConfigurationReadbackDTO>::Err(
        readiness_check.error());
  }

  auto* const concrete_channel =
      dynamic_cast<RetainedObservedStateChannel*>(old_channel.get());
  if (concrete_channel == nullptr) {
    return route_publication_error<core::RuntimeRouteConfigurationReadbackDTO>(
        "route_publication_gripper_state_channel_unavailable",
        "reconciled gripper publication requires the retained route-rebindable state channel",
        core::ErrorCode::kMissingReference);
  }

  const auto close_route = [&]() -> core::Result<void> {
    auto closed_configuration = configuration;
    closed_configuration.state = core::ActiveControlConfigurationStateDTO::
        kExternalOrUnknownUnreconciled;
    const auto closed = kernel_.publish_runtime_route_configuration(
        std::move(closed_configuration));
    if (!closed.ok()) {
      return core::Result<void>::Err(closed.error());
    }
    const auto closed_readback = kernel_.runtime_route_configuration_readback(
        configuration.control_domain_id,
        configuration.active_control_configuration_id,
        configuration.active_control_configuration_generation);
    if (!closed_readback.ok()) {
      return core::Result<void>::Err(closed_readback.error());
    }
    if (closed_readback.value() != closed.value() ||
        closed_readback.value().active_configuration.state !=
            core::ActiveControlConfigurationStateDTO::
                kExternalOrUnknownUnreconciled ||
        !closed_readback.value().readiness_rows.empty() ||
        closed_readback.value().operation_admission_open) {
      return core::Result<void>::Err(core::ErrorDTO{
          std::string(core::to_string(core::ErrorCode::kInvariantViolation)),
          "Kernel did not retain the fail-closed unreconciled gripper route",
          {{"reason", "route_publication_gripper_fail_closed_readback_mismatch"}}});
    }
    return core::Result<void>::Ok();
  };

  const auto rollback_physical_route = [&](const bool channel_rebound,
                                           const bool gripper_rebound)
      -> core::Result<void> {
    if (channel_rebound) {
      const auto channel_rollback =
          concrete_channel->RebindRoute(old_binding, clock_.now());
      if (!channel_rollback.ok()) {
        return channel_rollback;
      }
    }
    if (gripper_rebound) {
      const auto gripper_rollback = gripper_->RebindRoute(
          old_binding, old_channel, old_readiness);
      if (!gripper_rollback.ok()) {
        return gripper_rollback;
      }
    }
    const auto restored_binding = gripper_->binding();
    const auto restored_channel = gripper_->state_channel();
    const auto restored_readiness = gripper_->stop_readiness();
    if (restored_binding.value() != old_binding.value() ||
        restored_channel != old_channel ||
        restored_readiness.has_value() != old_readiness.has_value() ||
        (restored_readiness.has_value() &&
         restored_readiness->value() != old_readiness->value())) {
      return core::Result<void>::Err(core::ErrorDTO{
          std::string(core::to_string(core::ErrorCode::kInvariantViolation)),
          "retained gripper owner did not restore its exact route snapshot",
          {{"reason", "route_publication_gripper_rollback_mismatch"}}});
    }
    return core::Result<void>::Ok();
  };

  const auto transition_time_ns = clock_.now();
  const auto channel_rebind =
      concrete_channel->RebindRoute(candidate_binding, transition_time_ns);
  if (!channel_rebind.ok()) {
    return route_publication_error<core::RuntimeRouteConfigurationReadbackDTO>(
        "route_publication_gripper_state_rebind_failed",
        channel_rebind.error().message);
  }
  const auto gripper_rebind = gripper_->RebindRoute(
      candidate_binding, old_channel, verified.stop_readiness);
  if (!gripper_rebind.ok()) {
    const auto rollback = rollback_physical_route(true, false);
    const auto closed = close_route();
    if (!rollback.ok()) {
      return route_publication_error<core::RuntimeRouteConfigurationReadbackDTO>(
          "route_publication_gripper_rollback_failed",
          rollback.error().message +
              (closed.ok() ? std::string{} : "; fail-closed publication failed: " +
                                                closed.error().message));
    }
    if (!closed.ok()) {
      return route_publication_error<core::RuntimeRouteConfigurationReadbackDTO>(
          "route_publication_gripper_fail_closed_recovery_failed",
          closed.error().message);
    }
    return route_publication_error<core::RuntimeRouteConfigurationReadbackDTO>(
        "route_publication_gripper_backend_rebind_failed",
        gripper_rebind.error().message);
  }

  const auto gripper_registration = gripper_->registration();
  core::Result<KernelCandidate> cell_candidate =
      core::Result<KernelCandidate>::Err(core::ErrorDTO{
          std::string(core::to_string(core::ErrorCode::kInvariantViolation)),
          "cell Kernel candidate builder did not return",
          {{"reason", "route_publication_gripper_cell_builder_failed"}}});
  try {
    cell_candidate = cell_candidate_builder_(configuration,
                                              gripper_registration);
  } catch (const std::exception& exception) {
    cell_candidate = core::Result<KernelCandidate>::Err(core::ErrorDTO{
        std::string(core::to_string(core::ErrorCode::kInvariantViolation)),
        exception.what(),
        {{"reason", "route_publication_gripper_cell_builder_failed"}}});
  } catch (...) {
    cell_candidate = core::Result<KernelCandidate>::Err(core::ErrorDTO{
        std::string(core::to_string(core::ErrorCode::kInvariantViolation)),
        "cell Kernel candidate builder failed",
        {{"reason", "route_publication_gripper_cell_builder_failed"}}});
  }
  if (!cell_candidate.ok()) {
    const auto rollback = rollback_physical_route(true, true);
    if (!rollback.ok()) {
      const auto closed = close_route();
      return route_publication_error<core::RuntimeRouteConfigurationReadbackDTO>(
          "route_publication_gripper_rollback_failed",
          rollback.error().message +
              (closed.ok() ? std::string{} : "; fail-closed publication failed: " +
                                                closed.error().message));
    }
    return core::Result<core::RuntimeRouteConfigurationReadbackDTO>::Err(
        cell_candidate.error());
  }

  if (cell_candidate.value().registrations.empty()) {
    const auto rollback = rollback_physical_route(true, true);
    if (!rollback.ok()) {
      const auto closed = close_route();
      return route_publication_error<core::RuntimeRouteConfigurationReadbackDTO>(
          "route_publication_gripper_rollback_failed",
          rollback.error().message +
              (closed.ok() ? std::string{} : "; fail-closed publication failed: " +
                                                closed.error().message));
    }
    return route_publication_error<core::RuntimeRouteConfigurationReadbackDTO>(
        "route_publication_cell_registration_set_invalid",
        "stable cell owner supplied an empty Kernel registration set",
        core::ErrorCode::kInvalidRequest);
  }
  std::size_t exact_gripper_registrations = 0U;
  for (const auto& registration : cell_candidate.value().registrations) {
    if (registration.binding.value() != gripper_registration.binding.value()) {
      continue;
    }
    ++exact_gripper_registrations;
    const bool readiness_matches =
        registration.stop_readiness.has_value() &&
        registration.stop_readiness->value() == verified.stop_readiness.value();
    if (registration.engine != gripper_registration.engine ||
        registration.stop_execution_port !=
            gripper_registration.stop_execution_port ||
        !readiness_matches ||
        registration.retained_default_stop_limits !=
            gripper_registration.retained_default_stop_limits ||
        registration.stop_node_category !=
            gripper_registration.stop_node_category ||
        registration.intrinsic_direct_control !=
            gripper_registration.intrinsic_direct_control) {
      exact_gripper_registrations = 0U;
      break;
    }
  }
  if (exact_gripper_registrations != 1U) {
    const auto rollback = rollback_physical_route(true, true);
    if (!rollback.ok()) {
      const auto closed = close_route();
      return route_publication_error<core::RuntimeRouteConfigurationReadbackDTO>(
          "route_publication_gripper_rollback_failed",
          rollback.error().message +
              (closed.ok() ? std::string{} : "; fail-closed publication failed: " +
                                                closed.error().message));
    }
    return route_publication_error<core::RuntimeRouteConfigurationReadbackDTO>(
        "route_publication_cell_gripper_registration_mismatch",
        "cell Kernel candidate does not contain exactly one exact gripper registration",
        core::ErrorCode::kInvalidRequest);
  }

  const auto published = kernel_.publish_runtime_route_configuration(
      configuration, std::move(cell_candidate.value().registrations),
      std::move(cell_candidate.value().participant_resolver));
  if (!published.ok()) {
    const auto rollback = rollback_physical_route(true, true);
    const auto closed = close_route();
    if (!rollback.ok() || !closed.ok()) {
      return route_publication_error<core::RuntimeRouteConfigurationReadbackDTO>(
          "route_publication_gripper_fail_closed_recovery_failed",
          (!rollback.ok() ? rollback.error().message : std::string{}) +
              (!rollback.ok() && !closed.ok() ? "; " : std::string{}) +
              (!closed.ok() ? closed.error().message : std::string{}));
    }
    return published;
  }

  const auto readback = kernel_.runtime_route_configuration_readback(
      configuration.control_domain_id,
      configuration.active_control_configuration_id,
      configuration.active_control_configuration_generation);
  const std::vector<core::RouteReadinessPublicationRowDTO> expected_rows{
      verified.readiness_row};
  if (!readback.ok() || readback.value() != published.value() ||
      readback.value().active_configuration != configuration ||
      readback.value().readiness_rows != expected_rows ||
      !readback.value().operation_admission_open) {
    const auto closed = close_route();
    const auto rollback = rollback_physical_route(true, true);
    return route_publication_error<core::RuntimeRouteConfigurationReadbackDTO>(
        "route_publication_gripper_independent_readback_mismatch",
        (!readback.ok() ? readback.error().message
                        : "Kernel readback differs from the exact reconciled gripper publication") +
            (!closed.ok() ? "; fail-closed publication failed: " +
                                closed.error().message
                          : std::string{}) +
            (!rollback.ok() ? "; physical route rollback failed: " +
                                  rollback.error().message
                            : std::string{}));
  }
  return readback;
}

core::Result<core::RuntimeRouteConfigurationReadbackDTO>
RetainedNativeGripperRoutePublicationOwner::
    AdoptAndReadBackRouteConfigurationPredecessor(
        const core::ActiveControlConfigurationDTO& configuration) {
  if (!configuration.locally_valid() ||
      configuration.state !=
          core::ActiveControlConfigurationStateDTO::kReconciled ||
      configuration.binding_routes.size() != 1U ||
      !cell_candidate_builder_) {
    return route_publication_error<core::RuntimeRouteConfigurationReadbackDTO>(
        "route_predecessor_gripper_input_invalid",
        "gripper predecessor adoption requires one exact retained route and cell builder",
        core::ErrorCode::kInvalidRequest);
  }
  std::lock_guard lock(mutex_);
  const auto old_binding = gripper_->binding();
  const auto old_readiness = gripper_->stop_readiness();
  const auto channel = gripper_->state_channel();
  auto* const concrete_channel =
      dynamic_cast<RetainedObservedStateChannel*>(channel.get());
  if (!old_binding.value().control_path.has_value() ||
      concrete_channel == nullptr) {
    return route_publication_error<core::RuntimeRouteConfigurationReadbackDTO>(
        "route_predecessor_gripper_owner_missing",
        "gripper predecessor has no exact route-rebindable owner",
        core::ErrorCode::kMissingReference);
  }
  const auto& old_path = *old_binding.value().control_path;
  const auto& selected = configuration.binding_routes.front();
  if (old_path.control_domain_id != configuration.control_domain_id ||
      old_path.control_domain_revision !=
          configuration.control_domain_revision ||
      old_path.controller_topology_id != configuration.controller_topology_id ||
      old_path.topology_revision != configuration.topology_revision ||
      old_path.provider_runtime_unit_id !=
          configuration.source_runtime_unit_id ||
      old_path.control_binding_id != selected.control_binding_id ||
      old_path.control_binding_revision !=
          selected.control_binding_revision) {
    return route_publication_error<core::RuntimeRouteConfigurationReadbackDTO>(
        "route_predecessor_gripper_binding_mismatch",
        "durable gripper predecessor crosses its retained binding",
        core::ErrorCode::kInvalidRequest);
  }
  auto value = old_binding.value();
  value.control_path->control_route_id = selected.control_route_id;
  value.control_path->activation_set_id = configuration.activation_set_id;
  value.control_path->active_control_configuration_id =
      configuration.active_control_configuration_id;
  value.control_path->active_control_configuration_generation =
      configuration.active_control_configuration_generation;
  value.control_path->runtime_scale_qualification.reset();
  auto predecessor =
      core::verify_runtime_participant_binding(std::move(value));
  if (!predecessor.ok()) {
    return core::Result<core::RuntimeRouteConfigurationReadbackDTO>::Err(
        predecessor.error());
  }

  const auto rollback = [&]() -> core::Result<void> {
    auto reverted_channel = concrete_channel->RebindRoute(old_binding, clock_.now());
    if (!reverted_channel.ok()) return reverted_channel;
    return gripper_->RebindRoute(old_binding, channel, old_readiness);
  };
  auto rebound_channel =
      concrete_channel->RebindRoute(predecessor.value(), clock_.now());
  if (!rebound_channel.ok()) {
    return core::Result<core::RuntimeRouteConfigurationReadbackDTO>::Err(
        rebound_channel.error());
  }
  auto rebound_owner =
      gripper_->RebindPredecessorRoute(predecessor.value(), channel);
  if (!rebound_owner.ok()) {
    const auto reverted = rollback();
    return route_publication_error<core::RuntimeRouteConfigurationReadbackDTO>(
        reverted.ok() ? "route_predecessor_gripper_rebind_failed"
                      : "route_predecessor_gripper_rollback_failed",
        rebound_owner.error().message +
            (reverted.ok() ? std::string{}
                           : "; rollback failed: " + reverted.error().message));
  }
  core::Result<KernelCandidate> candidate =
      core::Result<KernelCandidate>::Err(core::ErrorDTO{
          std::string(core::to_string(core::ErrorCode::kInvariantViolation)),
          "gripper predecessor cell builder did not return",
          {{"reason", "route_predecessor_gripper_builder_failed"}}});
  try {
    candidate = cell_candidate_builder_(configuration, gripper_->registration());
  } catch (const std::exception& error) {
    candidate = core::Result<KernelCandidate>::Err(core::ErrorDTO{
        std::string(core::to_string(core::ErrorCode::kInvariantViolation)),
        error.what(), {{"reason", "route_predecessor_gripper_builder_failed"}}});
  } catch (...) {
    candidate = core::Result<KernelCandidate>::Err(core::ErrorDTO{
        std::string(core::to_string(core::ErrorCode::kInvariantViolation)),
        "gripper predecessor cell builder threw",
        {{"reason", "route_predecessor_gripper_builder_failed"}}});
  }
  if (!candidate.ok()) {
    const auto reverted = rollback();
    return route_publication_error<core::RuntimeRouteConfigurationReadbackDTO>(
        reverted.ok() ? "route_predecessor_gripper_builder_failed"
                      : "route_predecessor_gripper_rollback_failed",
        candidate.error().message +
            (reverted.ok() ? std::string{}
                           : "; rollback failed: " + reverted.error().message));
  }
  auto adopted = kernel_.adopt_runtime_route_configuration_predecessor(
      configuration, std::move(candidate.value().registrations),
      std::move(candidate.value().participant_resolver));
  if (!adopted.ok()) {
    const auto reverted = rollback();
    return route_publication_error<core::RuntimeRouteConfigurationReadbackDTO>(
        reverted.ok() ? "route_predecessor_gripper_kernel_refused"
                      : "route_predecessor_gripper_rollback_failed",
        adopted.error().message +
            (reverted.ok() ? std::string{}
                           : "; rollback failed: " + reverted.error().message));
  }
  auto readback = kernel_.runtime_route_configuration_readback(
      configuration.control_domain_id,
      configuration.active_control_configuration_id,
      configuration.active_control_configuration_generation);
  if (!readback.ok() || readback.value() != adopted.value() ||
      readback.value().operation_admission_open ||
      !readback.value().readiness_rows.empty()) {
    return route_publication_error<core::RuntimeRouteConfigurationReadbackDTO>(
        "route_predecessor_gripper_readback_mismatch",
        readback.ok() ? "gripper predecessor did not read back command-closed"
                      : readback.error().message);
  }
  return readback;
}

}  // namespace manyforge::roboplan_backend
