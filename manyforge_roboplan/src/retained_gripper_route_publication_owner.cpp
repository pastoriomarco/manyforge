#include <manyforge_roboplan/retained_cell_runtime.hpp>
#include <manyforge_roboplan/retained_gripper_route_publication_owner.hpp>
#include <manyforge_roboplan/retained_execution_host.hpp>

#include <algorithm>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <set>
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

RetainedGripperRoutePublicationOwner::
    RetainedGripperRoutePublicationOwner(
        std::shared_ptr<RetainedGripperRuntime> gripper,
        core::IClock& clock, core::ResourceAddressedExecutionKernel& kernel,
        CellKernelCandidateBuilder cell_candidate_builder, RetainedCellRuntime* cell_owner)
    : gripper_(std::move(gripper)), clock_(clock), kernel_(kernel),
      cell_candidate_builder_(std::move(cell_candidate_builder)), cell_owner_(cell_owner) {
  if (cell_owner_ && &cell_owner_->addressed_kernel() != &kernel_)
    throw std::invalid_argument("gripper route seam names another stable cell Kernel");
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

void RetainedGripperRoutePublicationOwner::BindParticipantPeers(
    const std::shared_ptr<RetainedGripperRoutePublicationOwner>& root,
    std::map<core::ControlDomainId,
        std::shared_ptr<RetainedGripperRoutePublicationOwner>> peers) {
  if (root.get() != this || !participant_peers_.empty() ||
      transition_mutex_ != &mutex_)
    throw std::invalid_argument("scalar route peer map requires its exact unjoined root");
  const auto own = gripper_->binding();
  for (const auto& [domain, peer] : peers) {
    if (!peer || peer.get() == this || &peer->kernel_ != &kernel_ ||
        &peer->clock_ != &clock_ || peer->cell_owner_ != cell_owner_ ||
        !peer->participant_peers_.empty() || peer->transition_mutex_ != &peer->mutex_)
      throw std::invalid_argument("scalar route peer map has a foreign or cyclic owner");
    const auto binding = peer->gripper_->binding();
    if (!binding.value().control_path ||
        binding.value().control_path->control_domain_id != domain ||
        (own.value().control_path && own.value().control_path->control_domain_id == domain))
      throw std::invalid_argument("scalar route peer map repeats or misnames a domain owner");
  }
  for (const auto& [domain, peer] : peers) {
    static_cast<void>(domain);
    peer->transition_owner_ = root;
    peer->transition_mutex_ = &mutex_;
  }
  participant_peers_.swap(peers);
}

core::Result<std::shared_ptr<void>>
RetainedGripperRoutePublicationOwner::PinTransitionOwner() const {
  if (transition_mutex_ == &mutex_)
    return core::Result<std::shared_ptr<void>>::Ok({});
  auto owner = transition_owner_.lock();
  if (!owner)
    return route_publication_error<std::shared_ptr<void>>(
        "route_publication_cell_owner_missing", "scalar route transition lost its exact cell owner",
        core::ErrorCode::kMissingReference);
  return core::Result<std::shared_ptr<void>>::Ok(std::move(owner));
}

core::Result<std::vector<core::RuntimeParticipantBinding>>
RetainedGripperRoutePublicationOwner::ReadCurrentPhysicalParticipants(
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
  if (configuration.binding_routes.empty()) {
    return route_publication_error<
        std::vector<core::RuntimeParticipantBinding>>(
        "route_publication_gripper_scope_invalid",
        "gripper route publication requires every exact installed binding",
        core::ErrorCode::kInvalidRequest);
  }

  const auto owner_binding = gripper_->binding();
  if (owner_binding.value().control_path &&
      owner_binding.value().control_path->control_domain_id != configuration.control_domain_id) {
    const auto peer = participant_peers_.find(configuration.control_domain_id);
    if (peer != participant_peers_.end())
      return peer->second->ReadCurrentPhysicalParticipants(configuration);
  }
  // Root transition -> scalar-local -> actual gripper mutex. Delegation above
  // never holds the shared nonrecursive mutex while the peer acquires it.
  const auto transition_owner = PinTransitionOwner();
  if (!transition_owner.ok()) return core::Result<std::vector<core::RuntimeParticipantBinding>>::Err(transition_owner.error());
  std::unique_lock transition_lock(*transition_mutex_);
  std::unique_lock local_lock(mutex_, std::defer_lock);
  if (transition_mutex_ != &mutex_) local_lock.lock();
  const auto installed_owner = ValidateInstalledOwner(transition_owner.value());
  if (!installed_owner.ok()) return core::Result<std::vector<core::RuntimeParticipantBinding>>::Err(installed_owner.error());
  if (configuration.binding_routes.size() != gripper_->registrations().size())
    return route_publication_error<std::vector<core::RuntimeParticipantBinding>> (
        "route_publication_gripper_scope_invalid", "Selected route omits or adds an installed gripper binding",
        core::ErrorCode::kInvalidRequest);
  const auto binding = gripper_->binding();
  const auto& value = binding.value();
  if (!value.control_path.has_value()) {
    return route_publication_error<
        std::vector<core::RuntimeParticipantBinding>>(
        "route_publication_gripper_binding_missing",
        "retained gripper no longer has a current control path",
        core::ErrorCode::kMissingReference);
  }
  std::vector<core::RuntimeParticipantBinding> representatives;
  for (const auto& registration : gripper_->registrations()) {
    const auto& current = registration.binding.value();
    if (!current.control_path)
      return route_publication_error<std::vector<core::RuntimeParticipantBinding>>(
          "route_publication_physical_binding_missing", "Gripper representative lost its control path");
    const auto& route_path = *current.control_path;
    const auto selected = std::find_if(configuration.binding_routes.begin(), configuration.binding_routes.end(),
        [&](const auto& row) { return row.control_binding_id == route_path.control_binding_id; });
    if (route_path.control_domain_id != configuration.control_domain_id ||
        selected == configuration.binding_routes.end() ||
        route_path.control_binding_revision != selected->control_binding_revision)
      return route_publication_error<std::vector<core::RuntimeParticipantBinding>>(
          "route_publication_physical_binding_missing", "Gripper does not own the complete selected binding set");
    representatives.push_back(registration.binding);
  }
  return core::Result<std::vector<core::RuntimeParticipantBinding>>::Ok(std::move(representatives));
}

core::Result<core::RuntimeRouteConfigurationReadbackDTO>
RetainedGripperRoutePublicationOwner::
    PublishAndReadBackRouteConfiguration(
        const core::ActiveControlConfigurationDTO& configuration,
        std::vector<RouteReadinessParticipant> participants) {
  if (!configuration.locally_valid()) {
    return route_publication_error<core::RuntimeRouteConfigurationReadbackDTO>(
        "route_publication_configuration_invalid",
        "native gripper route publication has an invalid configuration",
        core::ErrorCode::kInvalidRequest);
  }
  if (configuration.binding_routes.empty()) {
    return route_publication_error<core::RuntimeRouteConfigurationReadbackDTO>(
        "route_publication_gripper_scope_invalid",
        "gripper route publication requires every exact installed binding",
        core::ErrorCode::kInvalidRequest);
  }

  const auto owner_binding = gripper_->binding();
  if (owner_binding.value().control_path &&
      owner_binding.value().control_path->control_domain_id != configuration.control_domain_id) {
    const auto peer = participant_peers_.find(configuration.control_domain_id);
    if (peer != participant_peers_.end())
      return peer->second->PublishAndReadBackRouteConfiguration(configuration, std::move(participants));
  }
  // Root transition -> scalar-local -> actual gripper mutex. Delegation above
  // never holds the shared nonrecursive mutex while the peer acquires it.
  const auto transition_owner = PinTransitionOwner();
  if (!transition_owner.ok()) return core::Result<core::RuntimeRouteConfigurationReadbackDTO>::Err(transition_owner.error());
  std::unique_lock transition_lock(*transition_mutex_);
  std::unique_lock local_lock(mutex_, std::defer_lock);
  if (transition_mutex_ != &mutex_) local_lock.lock();
  const auto installed_owner = ValidateInstalledOwner(transition_owner.value());
  if (!installed_owner.ok()) return core::Result<core::RuntimeRouteConfigurationReadbackDTO>::Err(installed_owner.error());
  if (configuration.binding_routes.size() != gripper_->registrations().size())
    return route_publication_error<core::RuntimeRouteConfigurationReadbackDTO> (
        "route_publication_gripper_scope_invalid", "Selected route omits or adds an installed gripper binding",
        core::ErrorCode::kInvalidRequest);
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
  if (participants.size() != configuration.binding_routes.size()) {
    return route_publication_error<core::RuntimeRouteConfigurationReadbackDTO>(
        "route_publication_participant_set_invalid",
        "gripper publication requires one readiness participant per selected binding",
        core::ErrorCode::kInvalidRequest);
  }

  const auto primary = std::find_if(participants.begin(), participants.end(), [&](const auto& row) {
    return row.participant_binding.value().control_path &&
        row.participant_binding.value().control_path->control_binding_id == old_binding.value().control_path->control_binding_id;
  });
  if (primary == participants.end())
    return route_publication_error<core::RuntimeRouteConfigurationReadbackDTO>(
        "route_publication_participant_set_invalid", "Readiness participants omit the primary command binding");
  std::vector<core::RuntimeParticipantBinding> representatives;
  std::vector<std::optional<core::RuntimeParticipantStopReadiness>> readiness;
  std::vector<core::RouteReadinessPublicationRowDTO> expected_rows;
  std::set<core::ControlBindingId> selected_ids;
  for (const auto& row : participants) {
    const auto& value = row.participant_binding.value();
    if (!value.control_path)
      return route_publication_error<core::RuntimeRouteConfigurationReadbackDTO>(
          "route_publication_participant_referent_mismatch", "Readiness alias has no control path");
    const auto& path = *value.control_path;
    const auto route = std::find_if(configuration.binding_routes.begin(), configuration.binding_routes.end(),
        [&](const auto& item) { return item.control_binding_id == path.control_binding_id; });
    const auto& qualified = row.stop_readiness.value();
    if (!selected_ids.insert(path.control_binding_id).second || route == configuration.binding_routes.end() ||
        path.control_binding_revision != route->control_binding_revision || path.control_route_id != route->control_route_id ||
        path.control_domain_id != configuration.control_domain_id || path.control_domain_revision != configuration.control_domain_revision ||
        path.controller_topology_id != configuration.controller_topology_id || path.topology_revision != configuration.topology_revision ||
        path.activation_set_id != configuration.activation_set_id || path.active_control_configuration_id != configuration.active_control_configuration_id ||
        path.active_control_configuration_generation != configuration.active_control_configuration_generation ||
        path.provider_runtime_unit_id != configuration.source_runtime_unit_id || path.provider_runtime_epoch != configuration.source_runtime_epoch ||
        row.readiness_row.controllable_component_id != value.controllable_component_id ||
        row.readiness_row.component_lifecycle_generation != qualified.component_lifecycle_generation ||
        row.readiness_row.control_binding_id != path.control_binding_id || row.readiness_row.control_binding_revision != path.control_binding_revision ||
        row.readiness_row.resolved_stop_base_plan_id != qualified.resolved_stop_base_plan.resolved_stop_base_plan_id)
      return route_publication_error<core::RuntimeRouteConfigurationReadbackDTO>(
          "route_publication_participant_referent_mismatch", "Readiness aliases differ from the complete selected route");
    representatives.push_back(row.participant_binding);
    readiness.push_back(row.stop_readiness);
    expected_rows.push_back(row.readiness_row);
  }
  auto prepared_registrations = gripper_->PrepareRegistrations(representatives, readiness);
  if (!prepared_registrations.ok())
    return core::Result<core::RuntimeRouteConfigurationReadbackDTO>::Err(prepared_registrations.error());
  std::sort(expected_rows.begin(), expected_rows.end(), core::CanonicalRouteReadinessPublicationRowLess);
  const auto& verified = *primary;
  const auto& candidate_binding = verified.participant_binding;
  const auto& candidate_value = candidate_binding.value();
  const auto& candidate_path = *candidate_value.control_path;
  const auto selected_route = std::find_if(configuration.binding_routes.begin(), configuration.binding_routes.end(),
      [&](const auto& row) { return row.control_binding_id == candidate_path.control_binding_id; });
  const auto& selected = *selected_route;
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
  const auto& gripper_registrations = prepared_registrations.value();
  core::Result<KernelCandidate> cell_candidate =
      core::Result<KernelCandidate>::Err(core::ErrorDTO{
          std::string(core::to_string(core::ErrorCode::kInvariantViolation)),
          "cell Kernel candidate builder did not return",
          {{"reason", "route_publication_gripper_cell_builder_failed"}}});
  try {
    cell_candidate = cell_candidate_builder_(configuration,
                                              gripper_registrations);
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
  for (const auto& expected : gripper_registrations) {
    const auto matches = std::count_if(cell_candidate.value().registrations.begin(), cell_candidate.value().registrations.end(),
        [&](const auto& row) {
          return row.binding.value() == expected.binding.value() && row.engine == expected.engine &&
              row.stop_execution_port == expected.stop_execution_port && row.stop_readiness.has_value() == expected.stop_readiness.has_value() &&
              (!row.stop_readiness || row.stop_readiness->value() == expected.stop_readiness->value()) &&
              row.intrinsic_direct_control == expected.intrinsic_direct_control &&
              row.retained_default_stop_limits == expected.retained_default_stop_limits;
        });
    if (matches != 1) exact_gripper_registrations = 0;
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

  auto adopted_registrations = cell_candidate.value().registrations;
  auto adopted_resolver = cell_candidate.value().participant_resolver;
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
  gripper_->AdoptVerifiedRegistrations(std::move(prepared_registrations.value()));
  if (cell_owner_)
    cell_owner_->AdoptVerifiedComponentReadinessSnapshot(
        std::move(adopted_registrations), std::move(adopted_resolver));
  if (cell_candidate.value().adopt_component_owners)
    cell_candidate.value().adopt_component_owners();
  return readback;
}

core::Result<core::RuntimeRouteConfigurationReadbackDTO>
RetainedGripperRoutePublicationOwner::
    AdoptAndReadBackRouteConfigurationPredecessor(
        const core::ActiveControlConfigurationDTO& configuration) {
  if (!configuration.locally_valid() ||
      configuration.state !=
          core::ActiveControlConfigurationStateDTO::kReconciled ||
      configuration.binding_routes.empty() ||
      !cell_candidate_builder_) {
    return route_publication_error<core::RuntimeRouteConfigurationReadbackDTO>(
        "route_predecessor_gripper_input_invalid",
        "gripper predecessor adoption requires one exact retained route and cell builder",
        core::ErrorCode::kInvalidRequest);
  }
  const auto owner_binding = gripper_->binding();
  if (owner_binding.value().control_path &&
      owner_binding.value().control_path->control_domain_id != configuration.control_domain_id) {
    const auto peer = participant_peers_.find(configuration.control_domain_id);
    if (peer != participant_peers_.end())
      return peer->second->AdoptAndReadBackRouteConfigurationPredecessor(configuration);
  }
  // Root transition -> scalar-local -> actual gripper mutex. Delegation above
  // never holds the shared nonrecursive mutex while the peer acquires it.
  const auto transition_owner = PinTransitionOwner();
  if (!transition_owner.ok()) return core::Result<core::RuntimeRouteConfigurationReadbackDTO>::Err(transition_owner.error());
  std::unique_lock transition_lock(*transition_mutex_);
  std::unique_lock local_lock(mutex_, std::defer_lock);
  if (transition_mutex_ != &mutex_) local_lock.lock();
  const auto installed_owner = ValidateInstalledOwner(transition_owner.value());
  if (!installed_owner.ok()) return core::Result<core::RuntimeRouteConfigurationReadbackDTO>::Err(installed_owner.error());
  if (configuration.binding_routes.size() != gripper_->registrations().size())
    return route_publication_error<core::RuntimeRouteConfigurationReadbackDTO> (
        "route_publication_gripper_scope_invalid", "Selected route omits or adds an installed gripper binding",
        core::ErrorCode::kInvalidRequest);
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
  const auto primary_route = std::find_if(configuration.binding_routes.begin(), configuration.binding_routes.end(),
      [&](const auto& row) { return row.control_binding_id == old_path.control_binding_id; });
  if (primary_route == configuration.binding_routes.end())
    return route_publication_error<core::RuntimeRouteConfigurationReadbackDTO>(
        "route_predecessor_gripper_binding_mismatch", "Durable route omits the primary command binding");
  const auto& selected = *primary_route;
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

  std::vector<core::RuntimeParticipantBinding> representatives;
  std::vector<std::optional<core::RuntimeParticipantStopReadiness>> cleared_readiness;
  for (const auto& row : gripper_->registrations()) {
    auto value = row.binding.value();
    if (!value.control_path)
      return route_publication_error<core::RuntimeRouteConfigurationReadbackDTO>(
          "route_predecessor_gripper_binding_mismatch", "Predecessor alias has no control path");
    auto& path = *value.control_path;
    const auto selected = std::find_if(configuration.binding_routes.begin(), configuration.binding_routes.end(),
        [&](const auto& route) { return route.control_binding_id == path.control_binding_id; });
    if (selected == configuration.binding_routes.end() || selected->control_binding_revision != path.control_binding_revision ||
        path.control_domain_id != configuration.control_domain_id || path.control_domain_revision != configuration.control_domain_revision ||
        path.controller_topology_id != configuration.controller_topology_id || path.topology_revision != configuration.topology_revision ||
        path.provider_runtime_unit_id != configuration.source_runtime_unit_id || path.provider_runtime_epoch != old_path.provider_runtime_epoch)
      return route_publication_error<core::RuntimeRouteConfigurationReadbackDTO>(
          "route_predecessor_gripper_binding_mismatch", "Durable predecessor crosses its complete installed alias set");
    path.control_route_id = selected->control_route_id;
    path.activation_set_id = configuration.activation_set_id;
    path.active_control_configuration_id = configuration.active_control_configuration_id;
    path.active_control_configuration_generation = configuration.active_control_configuration_generation;
    path.runtime_scale_qualification.reset();
    auto sealed = core::verify_runtime_participant_binding(std::move(value));
    if (!sealed.ok()) return core::Result<core::RuntimeRouteConfigurationReadbackDTO>::Err(sealed.error());
    representatives.push_back(std::move(sealed.value()));
    cleared_readiness.push_back(std::nullopt);
  }
  auto prepared_registrations = gripper_->PrepareRegistrations(representatives, cleared_readiness);
  if (!prepared_registrations.ok())
    return core::Result<core::RuntimeRouteConfigurationReadbackDTO>::Err(
        core::ErrorDTO{"route_predecessor_gripper_rebind_failed",
                       prepared_registrations.error().message,
                       {{"reason", prepared_registrations.error().code}}});

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
    candidate = cell_candidate_builder_(configuration, prepared_registrations.value());
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
  auto adopted_registrations = candidate.value().registrations;
  auto adopted_resolver = candidate.value().participant_resolver;
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
  gripper_->AdoptVerifiedRegistrations(std::move(prepared_registrations.value()));
  if (cell_owner_)
    cell_owner_->AdoptVerifiedComponentReadinessSnapshot(
        std::move(adopted_registrations), std::move(adopted_resolver));
  if (candidate.value().adopt_component_owners)
    candidate.value().adopt_component_owners();
  return readback;
}

}  // namespace manyforge::roboplan_backend
