#include "resource_addressed_execution_kernel_internal.hpp"

namespace manyforge::core {

Result<std::unique_ptr<ICellCloseAdmissionLease>>
ResourceAddressedExecutionKernel::AcquireCellCloseAdmissionLease() {
  // Ordinary Execute admission already takes this same lock before state
  // (the pending/admitted publication barrier). No submission/state lock or
  // callback is retained over the caller's journal transaction.
  std::unique_ptr<ICellCloseAdmissionLease> lease =
      std::make_unique<CellCloseAdmissionLease>(*this);
  return Result<std::unique_ptr<ICellCloseAdmissionLease>>::Ok(std::move(lease));
}

AuthoringAdmissionDTO ResourceAddressedExecutionKernel::GetAuthoringAdmission() const {
  return cell_admission_owner_->GetAuthoringAdmission();
}


Result<void> ResourceAddressedExecutionKernel::SetRouteRecoveryPending(bool pending) {
  std::lock_guard admission_lock(*admission_mutex_);
  return cell_admission_owner_->SetRouteRecoveryPending(pending);
}

Result<AuthoringAdmissionDTO> ResourceAddressedExecutionKernel::UpdateCellAdmissionReadiness(
    const CellAdmissionReadinessSnapshotDTO& snapshot) {
  std::lock_guard admission_lock(*admission_mutex_);
  auto updated = cell_admission_owner_->UpdateCellAdmissionReadiness(snapshot);
  if (updated.ok()) {
    std::lock_guard state_lock(state_mutex_);
    current_program_run_id_ =
        snapshot.program_cycle_status_current && snapshot.program_cycle_status.has_value()
            ? snapshot.program_cycle_status->program_run_id
            : std::nullopt;
  }
  return updated;
}

Result<CellAdmissionClaimDTO> ResourceAddressedExecutionKernel::AcquireCellAdmissionClaim(
    const CellAdmissionClaimIntentDTO& intent) {
  if (intent.claim_kind == CellAdmissionClaimKindDTO::kInterventionOrRecovery &&
      intent.owner_role == CellAdmissionOwnerRoleDTO::kComponentLifecycleOwner) {
    return reject<CellAdmissionClaimDTO>(
        "CCLC recovery requires exact Kernel predecessor verification",
        "cclc_route_recovery_requires_owner_reconciliation", ErrorCode::kInvalidRequest);
  }
  if (intent.claim_kind == CellAdmissionClaimKindDTO::kManualOperation) {
    return reject<CellAdmissionClaimDTO>(
        "manual claims are minted only by exact Kernel operation admission",
        "manual_operation_claim_requires_execute", ErrorCode::kInvalidRequest);
  }
  // A lifecycle claim is a whole-cell configuration mutation. Serialize the
  // physical work check with every command submission, then retain the
  // admission lock through the durable claim so Start cannot win in between.
  std::unique_lock<std::mutex> submit_lock(submission_mutex_, std::defer_lock);
  if (intent.claim_kind == CellAdmissionClaimKindDTO::kComponentLifecycle) {
    submit_lock.lock();
  }
  std::lock_guard admission_lock(*admission_mutex_);
  if (intent.claim_kind == CellAdmissionClaimKindDTO::kComponentLifecycle) {
    std::lock_guard state_lock(state_mutex_);
    // Every blocking condition and the identities it concerns, so a caller
    // can tell exactly what must settle; nothing is retried or stopped.
    std::vector<std::string> blocking;
    if (pending_program_run_admission_.has_value()) blocking.emplace_back("program_run_admission_pending");
    if (!active_operations_.empty()) blocking.emplace_back("operations_active");
    if (operation_control_reconfiguration_busy_locked()) blocking.emplace_back("operation_control_busy");
    if (motion_admission_unreconciled_) blocking.emplace_back("motion_admission_unreconciled");
    if (manager_liveness_motion_inhibited_) blocking.emplace_back("manager_liveness_inhibited");
    std::vector<std::string> busy_resources;
    for (const auto& slot : resource_slots_->snapshot()) {
      if (slot.state != ResourceSlotState::kFree || slot.owner.has_value()) {
        busy_resources.push_back(slot.resource_id.value());
      }
    }
    if (!busy_resources.empty()) blocking.emplace_back("resources_not_free");
    if (!blocking.empty()) {
      const auto join = [](const auto& values) {
        std::string joined;
        for (std::size_t index = 0U; index < values.size() && index < 16U; ++index) {
          if (!joined.empty()) joined += ',';
          joined += values[index];
        }
        return joined;
      };
      std::vector<std::string> operations;
      for (const auto& [operation_id, operation] : active_operations_) {
        static_cast<void>(operation);
        operations.push_back(operation_id.value());
      }
      auto rejected = reject<CellAdmissionClaimDTO>(
          "lifecycle mutation requires settled physical and Program work",
          blocking.size() == 1U && !busy_resources.empty()
              ? "component_lifecycle_resources_not_settled"
              : "component_lifecycle_effects_not_settled");
      auto error = rejected.error();
      error.details["blocking_conditions"] = join(blocking);
      if (!operations.empty()) error.details["active_operation_ids"] = join(operations);
      if (!busy_resources.empty()) error.details["busy_resource_ids"] = join(busy_resources);
      return Result<CellAdmissionClaimDTO>::Err(std::move(error));
    }
  }
  auto acquired = cell_admission_owner_->AcquireCellAdmissionClaim(intent);
  if (acquired.ok() && intent.claim_kind == CellAdmissionClaimKindDTO::kProgramCycle) {
    refresh_execution_claims();
  }
  return acquired;
}

Result<CellAdmissionClaimDTO>
ResourceAddressedExecutionKernel::AcquireRouteRecoveryCellAdmissionClaim(
    const CellAdmissionClaimIntentDTO& intent, const ActiveControlConfigurationDTO& predecessor) {
  if (!predecessor.locally_valid() ||
      predecessor.state != ActiveControlConfigurationStateDTO::kReconciled) {
    return reject<CellAdmissionClaimDTO>("route recovery predecessor is invalid",
                                         "route_recovery_predecessor_invalid",
                                         ErrorCode::kInvalidRequest);
  }
  std::lock_guard submit_lock(submission_mutex_);
  std::lock_guard admission_lock(*admission_mutex_);
  return cell_admission_owner_->AcquireRouteRecoveryClaim(
      intent, [&](const auto& offered, const auto& parent) -> Result<void> {
        std::lock_guard state_lock(state_mutex_);
        const auto found =
            runtime_route_configuration_readbacks_.find(predecessor.control_domain_id);
        if (parent ||
            offered.scope.configurations !=
                std::vector<CellAdmissionConfigurationScopeDTO>{
                    {predecessor.control_domain_id, predecessor.active_control_configuration_id,
                     predecessor.active_control_configuration_generation}} ||
            found == runtime_route_configuration_readbacks_.end() ||
            found->second.active_configuration != predecessor ||
            found->second.operation_admission_open || !found->second.readiness_rows.empty() ||
            !runtime_route_configuration_publication_required_.contains(
                predecessor.control_domain_id)) {
          return reject<void>("route recovery has no exact admission-closed adopted predecessor",
                              "route_recovery_predecessor_not_current");
        }
        const bool active_stop_fault = is_active_stop_fault_latch(active_stop_fault_latch_);
        if ((!active_stop_fault && motion_admission_closed_) || motion_admission_unreconciled_ ||
            manager_liveness_motion_inhibited_ || !active_operations_.empty() ||
            operation_control_reconfiguration_busy_locked()) {
          return reject<void>("route recovery conflicts with resident or unresolved work",
                              "route_recovery_effects_not_settled");
        }
        const auto slots = resource_slots_->snapshot();
        if (std::any_of(slots.begin(), slots.end(), [](const auto& slot) {
              return slot.state != ResourceSlotState::kFree || slot.owner.has_value();
            })) {
          return reject<void>("route recovery has reserved or unknown physical effects",
                              "route_recovery_resources_not_settled");
        }
        std::optional<RuntimeEpoch> replacement_epoch;
        for (const auto& registration : registrations_) {
          const auto& path = registration.binding.value().control_path;
          if (!path || path->control_domain_id != predecessor.control_domain_id) continue;
          if (registration.stop_readiness ||
              path->provider_runtime_unit_id != predecessor.source_runtime_unit_id ||
              path->provider_runtime_epoch == predecessor.source_runtime_epoch ||
              (replacement_epoch && *replacement_epoch != path->provider_runtime_epoch)) {
            return reject<void>("route recovery has no unique replacement runtime epoch",
                                "route_recovery_epoch_not_current");
          }
          replacement_epoch = path->provider_runtime_epoch;
        }
        if (!replacement_epoch) {
          return reject<void>("route recovery has no current physical participant",
                              "route_recovery_participant_missing");
        }
        return Result<void>::Ok();
      });
}

Result<void> ResourceAddressedExecutionKernel::validate_selected_bring_up_offer_identity(
    const CellAdmissionClaimIntentDTO& intent,
    const SelectedComponentBringUpOfferDTO& offer) const {
  const auto scope = SelectedComponentBringUpAdmissionScope(offer);
  const auto fence_scope = SelectedComponentBringUpFenceScope(offer);
  if (!scope.ok()) return Result<void>::Err(scope.error());
  if (!fence_scope.ok()) return Result<void>::Err(fence_scope.error());
  try {
    if (intent.owner_transaction_id != offer.lifecycle_transaction_id.value() ||
        intent.request_id != offer.request_context.request_id ||
        intent.owner_epoch != offer.request_context.request_owner_epoch ||
        intent.scope != scope.value() ||
        intent.request_payload_digest !=
            ContentDigest(detail::SelectedComponentBringUpSemanticOfferDigest(offer)) ||
        offer.fence_admission.echoed_query.affected_scope != fence_scope.value() ||
        offer.fence_admission.active_projection_revision !=
            offer.predecessor.active_publication_receipt.request.candidate_projection_revision ||
        offer.fence_admission.execution_host_epoch.publication_generation !=
            offer.predecessor.active_publication_receipt.publication_generation ||
        offer.fence_admission.active_public_pointer_digest !=
            offer.predecessor.active_publication_receipt.durable_public_pointer_content_digest ||
        offer.fence_admission.affected_active_operation_count != 0U ||
        offer.fence_admission.affected_queued_operation_count != 0U) {
      return reject<void>("selected bring-up offer identity is not exact",
                          "selected_bring_up_offer_invalid", ErrorCode::kInvalidRequest);
    }
  } catch (const std::exception&) {
    return reject<void>("selected bring-up offer cannot be encoded",
                        "selected_bring_up_offer_invalid", ErrorCode::kInvalidRequest);
  }
  return Result<void>::Ok();
}

Result<void> ResourceAddressedExecutionKernel::validate_selected_bring_up_predecessor_locked(
    const CellAdmissionClaimIntentDTO& intent, const SelectedComponentBringUpOfferDTO& offer) {
  if (auto valid = validate_selected_bring_up_offer_identity(intent, offer); !valid.ok())
    return valid;
  std::lock_guard state_lock(state_mutex_);
  const auto& component = offer.predecessor.controllable_component_id;
  const auto readiness = component_command_readiness_.find(component);
  const auto containment =
      std::find_if(incident_containments_.begin(), incident_containments_.end(),
                   [&](const auto& row) { return row.incident_id == offer.incident_id; });
  if (parent_program_cycle_claim(intent) || !is_active_stop_fault_latch(active_stop_fault_latch_) ||
      !motion_admission_closed_ || readiness == component_command_readiness_.end() ||
      readiness->second.component_lifecycle_generation !=
          offer.predecessor.component_lifecycle_generation ||
      containment == incident_containments_.end() || containment->receipt != offer.containment ||
      containment->controllable_component_id != component ||
      offer.fence_admission.execution_host_epoch.request_owner_epoch !=
          config_.request_owner_epoch) {
    return reject<void>("selected bring-up has no exact contained failed predecessor",
                        "selected_bring_up_predecessor_not_current");
  }
  const auto terminal = std::find_if(
      stop_terminal_history_.begin(), stop_terminal_history_.end(), [&](const auto& record) {
        return record->status.accepted.stop_id == offer.containment.stop_id &&
               record->status.accepted.stop_generation == offer.containment.stop_generation;
      });
  if (terminal == stop_terminal_history_.end() || !(*terminal)->status.unsettled.empty() ||
      std::any_of(stop_transactions_.begin(), stop_transactions_.end(),
                  [](const auto& stop) { return stop->active && !stop->terminal; }) ||
      pending_program_run_admission_ || !active_operations_.empty() ||
      operation_control_reconfiguration_busy_locked() || motion_admission_unreconciled_ ||
      manager_liveness_motion_inhibited_) {
    return reject<void>("selected bring-up requires settled resident containment and work",
                        "selected_bring_up_effects_not_settled");
  }
  const auto slots = resource_slots_->snapshot();
  if (std::any_of(slots.begin(), slots.end(), [](const auto& slot) {
        return slot.state != ResourceSlotState::kFree || slot.owner.has_value();
      })) {
    return reject<void>("selected bring-up has reserved or unknown physical effects",
                        "selected_bring_up_resources_not_settled");
  }
  return validate_bring_up_component_closure_locked(
      offer.predecessor, offer.candidate_participants, offer.fence_admission);
}

Result<void> ResourceAddressedExecutionKernel::validate_bring_up_component_closure_locked(
    const ComponentLifecycleCurrentSnapshotDTO& predecessor,
    const std::vector<RuntimeParticipantBindingDTO>& candidates,
    const ConfigurationFenceAdmissionSnapshotDTO& admission) {
  const auto& component = predecessor.controllable_component_id;
  std::vector<ResourceId> current_resources;
  std::set<ControlBindingId> current_bindings;
  for (const auto& registration : registrations_) {
    const auto& binding = registration.binding.value();
    if (binding.controllable_component_id != component || !binding.control_path) continue;
    const auto domain = std::find_if(
        predecessor.domains.begin(), predecessor.domains.end(), [&](const auto& row) {
          return row.control_domain_id == binding.control_path->control_domain_id;
        });
    if (domain == predecessor.domains.end()) {
      return reject<void>("selected bring-up omits an owned predecessor domain",
                          "selected_bring_up_registration_not_current");
    }
  }
  for (std::size_t index = 0; index < predecessor.domains.size(); ++index) {
    const auto& domain = predecessor.domains[index];
    const auto& provider = predecessor.provider_occurrences[index];
    const auto adopted = runtime_route_configuration_readbacks_.find(domain.control_domain_id);
    if (!domain.active_control_configuration ||
        adopted == runtime_route_configuration_readbacks_.end() ||
        adopted->second.active_configuration != *domain.active_control_configuration) {
      return reject<void>("selected bring-up predecessor configuration is not adopted",
                          "selected_bring_up_configuration_not_current");
    }
    for (const auto& registration : registrations_) {
      const auto& binding = registration.binding.value();
      if (binding.controllable_component_id != component || !binding.control_path ||
          binding.control_path->control_domain_id != domain.control_domain_id)
        continue;
      const auto& path = *binding.control_path;
      const auto control_binding =
          std::find_if(domain.current_control_bindings.begin(),
                       domain.current_control_bindings.end(), [&](const auto& row) {
                         return row.control_binding_id == path.control_binding_id &&
                                row.control_binding_revision == path.control_binding_revision &&
                                row.control_route_id == path.control_route_id;
                       });
      if (binding.retained_bundle_id !=
              admission.active_projection_revision.retained_bundle_id ||
          binding.graph_revision !=
              admission.active_projection_revision.graph_revision ||
          path.control_domain_revision != domain.control_domain_revision ||
          path.controller_topology_id != domain.controller_topology_id ||
          path.topology_revision != domain.topology_revision ||
          control_binding == domain.current_control_bindings.end() ||
          path.provider_instance_id != provider.provider_instance_id ||
          path.provider_manifest_id != provider.provider_manifest_id ||
          path.provider_manifest_revision != provider.provider_manifest_revision ||
          path.provider_runtime_unit_id != provider.provider_runtime_unit_id ||
          path.provider_runtime_epoch != provider.provider_runtime_epoch ||
          path.provider_runtime_unit_id !=
              admission.execution_host_epoch.runtime_unit_id ||
          path.provider_runtime_epoch != admission.execution_host_epoch.runtime_epoch) {
        return reject<void>("selected bring-up predecessor registration is not exact",
                            "selected_bring_up_registration_not_current");
      }
      current_bindings.insert(path.control_binding_id);
      for (const auto& resource : binding.resources)
        current_resources.push_back(resource.resource_id);
    }
    for (const auto& binding : domain.current_control_bindings) {
      if (!current_bindings.contains(binding.control_binding_id)) {
        return reject<void>("selected bring-up predecessor participant is missing",
                            "selected_bring_up_registration_not_current");
      }
    }
  }
  std::sort(current_resources.begin(), current_resources.end());
  current_resources.erase(std::unique(current_resources.begin(), current_resources.end()),
                          current_resources.end());
  std::vector<ResourceId> expected_resources;
  for (const auto& domain : predecessor.domains)
    expected_resources.insert(expected_resources.end(), domain.current_resources.begin(),
                              domain.current_resources.end());
  std::sort(expected_resources.begin(), expected_resources.end());
  expected_resources.erase(std::unique(expected_resources.begin(), expected_resources.end()),
                           expected_resources.end());
  if (current_resources != expected_resources) {
    return reject<void>("selected bring-up predecessor physical closure changed",
                        "selected_bring_up_resources_not_current");
  }
  return validate_bring_up_candidate_peer_closure_locked(component, candidates, admission);
}

Result<void> ResourceAddressedExecutionKernel::validate_bring_up_candidate_peer_closure_locked(
    const ControllableComponentId& component,
    const std::vector<RuntimeParticipantBindingDTO>& candidates,
    const ConfigurationFenceAdmissionSnapshotDTO& admission) {
  for (const auto& candidate : candidates) {
    const auto& path = *candidate.control_path;
    if (path.provider_runtime_unit_id !=
            admission.execution_host_epoch.runtime_unit_id ||
        path.provider_runtime_epoch != admission.execution_host_epoch.runtime_epoch) {
      return reject<void>("selected bring-up cannot change the shared host epoch",
                          "selected_bring_up_host_epoch_changed");
    }
    for (const auto& registration : registrations_) {
      const auto& other = registration.binding.value();
      if (other.controllable_component_id == component) continue;
      if (other.control_path &&
          (other.control_path->control_domain_id == path.control_domain_id ||
           other.control_path->control_binding_id == path.control_binding_id)) {
        return reject<void>("selected bring-up crosses a healthy peer domain or binding",
                            "selected_bring_up_nonselected_scope_changed");
      }
      for (const auto& resource : candidate.resources) {
        if (std::any_of(other.resources.begin(), other.resources.end(),
                        [&](const auto& row) { return row.resource_id == resource.resource_id; })) {
          return reject<void>("selected bring-up crosses a healthy peer resource",
                              "selected_bring_up_nonselected_scope_changed");
        }
      }
    }
  }
  return Result<void>::Ok();
}

Result<void> ResourceAddressedExecutionKernel::validate_apply_bring_up_current_locked(
    const CellAdmissionClaimIntentDTO& intent, const ApplyBringUpAdmissionOfferDTO& offer,
    const CellAdmissionReadinessSnapshotDTO& readiness) {
  // The aggregate producer currently erases which components contributed a
  // deficit. Do not subtract the offered subset or treat count equality as
  // exact attribution. This slice stays explicitly incomplete until that
  // source-qualified join is composed by the existing owners.
  if (readiness.blocking_fault_count != 0U || !readiness.deployment_converged ||
      readiness.required_component_total !=
          readiness.required_component_current_compatible_ready_converged) {
    return reject<void>("selected readiness deficits have no exact source-qualified attribution",
                        "apply_bring_up_readiness_attribution_unavailable");
  }
  if (!offer.parent.valid() || !offer.admission.presence_valid() ||
      intent.owner_transaction_id != offer.parent.owner.apply_id.value() ||
      intent.request_payload_digest != offer.parent.plan_digest ||
      offer.admission.execution_host_epoch.request_owner_epoch != config_.request_owner_epoch ||
      offer.admission.affected_active_operation_count != 0U ||
      offer.admission.affected_queued_operation_count != 0U ||
      offer.configured_components.empty() || offer.needed_components.empty() ||
      std::any_of(offer.configured_components.begin(), offer.configured_components.end(),
                  [](const auto& component) { return component.empty(); }) ||
      offer.configured_components.size() > kMaximumCellAdmissionScopeEntries ||
      offer.needed_components.size() > offer.configured_components.size() ||
      !std::is_sorted(offer.configured_components.begin(), offer.configured_components.end()) ||
      std::adjacent_find(offer.configured_components.begin(), offer.configured_components.end()) !=
          offer.configured_components.end()) {
    return reject<void>("Apply bring-up parent/current input is not exact",
                        "apply_bring_up_offer_invalid", ErrorCode::kInvalidRequest);
  }
  std::lock_guard state_lock(state_mutex_);
  if (pending_program_run_admission_ || !active_operations_.empty() ||
      operation_control_reconfiguration_busy_locked() || motion_admission_unreconciled_ ||
      manager_liveness_motion_inhibited_ ||
      std::any_of(stop_transactions_.begin(), stop_transactions_.end(),
                  [](const auto& stop) { return stop->active && !stop->terminal; })) {
    return reject<void>("Apply bring-up requires settled physical and Program work",
                        "apply_bring_up_effects_not_settled");
  }
  const auto slots = resource_slots_->snapshot();
  if (std::any_of(slots.begin(), slots.end(), [](const auto& slot) {
        return slot.state != ResourceSlotState::kFree || slot.owner.has_value();
      })) {
    return reject<void>("Apply bring-up has reserved or unknown physical effects",
                        "apply_bring_up_resources_not_settled");
  }
  ConfigurationFenceScopeDTO closure;
  std::map<ControlDomainId, ConfigurationFenceDomainScopeDTO> domains;
  std::map<ControlBindingId, ConfigurationFenceBindingScopeDTO> bindings;
  std::set<ResourceId> resources;
  std::map<ResourceId, ControllableComponentId> candidate_resource_owners;
  bool exact_latch = false;
  std::set<std::string> incidents;
  for (const auto& row : offer.needed_components) {
    const auto& component = row.controllable_component_id;
    if (component.empty() ||
        (!closure.controllable_component_ids.empty() &&
         !(closure.controllable_component_ids.back() < component)) ||
        !std::binary_search(offer.configured_components.begin(),
                            offer.configured_components.end(), component)) {
      return reject<void>("Apply bring-up needed set is not canonical/configured",
                          "apply_bring_up_offer_invalid", ErrorCode::kInvalidRequest);
    }
    closure.controllable_component_ids.push_back(component);
    if (row.candidate_participants.empty() ||
        row.candidate_participants.size() > kMaximumCellAdmissionScopeEntries) {
      return reject<void>("Apply bring-up candidate closure is absent or exceeds its bound",
                          "apply_bring_up_candidate_invalid", ErrorCode::kInvalidRequest);
    }
    for (const auto& candidate : row.candidate_participants) {
      const auto verified = verify_runtime_participant_binding(candidate);
      if (!verified.ok() || candidate.controllable_component_id != component ||
          !candidate.control_path) {
        return reject<void>("Apply bring-up candidate binding is not qualified",
                            "apply_bring_up_candidate_invalid", ErrorCode::kInvalidRequest);
      }
      // Preserve derive_stop_static_facts' existing physical attribution:
      // same-component aliases are valid; another component cannot own it.
      for (const auto& resource : candidate.resources) {
        const auto [owner, inserted] = candidate_resource_owners.emplace(resource.resource_id, component);
        if (!inserted && owner->second != component) {
          return reject<void>("candidate Stop resource belongs to multiple components",
                              "apply_bring_up_candidate_resource_owner_conflict");
        }
      }
    }
    if (row.kind == ApplyBringUpAdmissionComponentKindDTO::kInitial) {
      // Manager's original live caller proves the never-brought-up owner
      // classification. Kernel independently proves actual absent installation
      // and healthy ordinary prerequisites; absence alone is not authority.
      if (row.predecessor || row.recovery || is_active_stop_fault_latch(active_stop_fault_latch_) ||
          motion_admission_closed_ || component_command_readiness_.contains(component) ||
          std::any_of(registrations_.begin(), registrations_.end(), [&](const auto& current) {
            return current.binding.value().controllable_component_id == component;
          }) || std::any_of(incident_containments_.begin(), incident_containments_.end(),
                           [&](const auto& current) {
            return current.controllable_component_id == component;
          })) {
        return reject<void>("initial bring-up has an installed or faulted predecessor",
                            "apply_bring_up_initial_not_current");
      }
      if (auto current = validate_bring_up_candidate_peer_closure_locked(
              component, row.candidate_participants, offer.admission); !current.ok()) return current;
    } else {
      if (row.kind != ApplyBringUpAdmissionComponentKindDTO::kContainedRecovery ||
          !row.predecessor || !row.recovery || row.candidate_participants.empty() ||
          row.candidate_participants.size() > kMaximumCellAdmissionScopeEntries ||
          row.predecessor->controllable_component_id != component ||
          !VerifyComponentLifecycleCurrentSnapshot(*row.predecessor).ok() ||
          !incidents.insert(row.recovery->incident_id).second) {
        return reject<void>("Apply bring-up recovery row is incomplete",
                            "apply_bring_up_offer_invalid", ErrorCode::kInvalidRequest);
      }
      const auto& predecessor = *row.predecessor;
      const auto& containment = row.recovery->containment;
      const auto ready = component_command_readiness_.find(component);
      const auto incident = std::find_if(incident_containments_.begin(), incident_containments_.end(),
          [&](const auto& current) { return current.incident_id == row.recovery->incident_id; });
      const auto terminal = std::find_if(stop_terminal_history_.begin(), stop_terminal_history_.end(),
          [&](const auto& current) {
            return current->status.accepted.stop_id == containment.stop_id &&
                   current->status.accepted.stop_generation == containment.stop_generation;
          });
      if (!is_active_stop_fault_latch(active_stop_fault_latch_) || !motion_admission_closed_ ||
          ready == component_command_readiness_.end() ||
          ready->second.component_lifecycle_generation != predecessor.component_lifecycle_generation ||
          incident == incident_containments_.end() || incident->controllable_component_id != component ||
          incident->receipt != containment || terminal == stop_terminal_history_.end() ||
          !(*terminal)->status.unsettled.empty() ||
          predecessor.active_publication_receipt.publication_generation !=
              offer.admission.execution_host_epoch.publication_generation ||
          predecessor.active_publication_receipt.request.candidate_projection_revision !=
              offer.admission.active_projection_revision ||
          predecessor.active_publication_receipt.durable_public_pointer_content_digest !=
              offer.admission.active_public_pointer_digest) {
        return reject<void>("Apply bring-up has no exact settled contained predecessor",
                            "apply_bring_up_predecessor_not_current");
      }
      if (active_stop_fault_latch_->stop_id == containment.stop_id &&
          active_stop_fault_latch_->stop_generation == containment.stop_generation) {
        exact_latch = std::any_of(attributed_containment_stops_.begin(),
            attributed_containment_stops_.end(), [&](const auto& current) {
              return current.stop_id == containment.stop_id &&
                     current.stop_generation == containment.stop_generation &&
                     current.origin == StopFaultLatchOriginDTO::kComponentIncident;
            });
      }
      if (auto current = validate_bring_up_component_closure_locked(
              predecessor, row.candidate_participants, offer.admission); !current.ok()) return current;
      for (const auto& domain : predecessor.domains) {
        auto& entry = domains.try_emplace(
            domain.control_domain_id,
            ConfigurationFenceDomainScopeDTO{domain.control_domain_id, {}, {}}).first->second;
        entry.control_domain_id = domain.control_domain_id;
        entry.predecessor_control_domain_revision = domain.control_domain_revision;
        for (const auto& binding : domain.current_control_bindings) {
          auto& current = bindings.try_emplace(
              binding.control_binding_id,
              ConfigurationFenceBindingScopeDTO{binding.control_binding_id, {}, {}}).first->second;
          current.control_binding_id = binding.control_binding_id;
          current.predecessor_control_binding_revision = binding.control_binding_revision;
        }
        resources.insert(domain.current_resources.begin(), domain.current_resources.end());
      }
    }
    for (const auto& candidate : row.candidate_participants) {
      const auto& path = *candidate.control_path;
      auto& domain = domains.try_emplace(
          path.control_domain_id,
          ConfigurationFenceDomainScopeDTO{path.control_domain_id, {}, {}}).first->second;
      domain.control_domain_id = path.control_domain_id;
      if (domain.candidate_control_domain_revision &&
          domain.candidate_control_domain_revision != path.control_domain_revision) {
        return reject<void>("candidate domain closure conflicts", "apply_bring_up_candidate_invalid");
      }
      domain.candidate_control_domain_revision = path.control_domain_revision;
      auto& binding = bindings.try_emplace(
          path.control_binding_id,
          ConfigurationFenceBindingScopeDTO{path.control_binding_id, {}, {}}).first->second;
      binding.control_binding_id = path.control_binding_id;
      if (binding.candidate_control_binding_revision &&
          binding.candidate_control_binding_revision != path.control_binding_revision) {
        return reject<void>("candidate binding closure conflicts", "apply_bring_up_candidate_invalid");
      }
      binding.candidate_control_binding_revision = path.control_binding_revision;
      for (const auto& resource : candidate.resources) resources.insert(resource.resource_id);
    }
  }
  for (const auto& [id, domain] : domains) { static_cast<void>(id); closure.domains.push_back(domain); }
  for (const auto& [id, binding] : bindings) { static_cast<void>(id); closure.bindings.push_back(binding); }
  closure.resources.assign(resources.begin(), resources.end());
  if ((is_active_stop_fault_latch(active_stop_fault_latch_) && !exact_latch) ||
      closure != offer.admission.echoed_query.affected_scope) {
    return reject<void>("Apply bring-up crosses its exact contained selected scope",
                        "apply_bring_up_scope_not_exact");
  }
  return Result<void>::Ok();
}

Result<CellAdmissionClaimDTO>
ResourceAddressedExecutionKernel::AcquireConfigurationApplyCellAdmissionClaim(
    const CellAdmissionClaimIntentDTO& intent,
    const CellAdmissionConfigurationApplyValidator& validate) {
  std::lock_guard submit_lock(submission_mutex_);
  std::lock_guard admission_lock(*admission_mutex_);
  return cell_admission_owner_->AcquireConfigurationApplyClaim(intent, validate);
}

Result<AuthoringAdmissionDTO>
ResourceAddressedExecutionKernel::ReleaseTerminalApplyClaimsAfterSuccess(
    const CellAdmissionClaimDTO& fresh_claim,
    const CellAdmissionClaimReleaseDTO& fresh_success_release,
    const CellAdmissionConfigurationApplySuccessValidator& validate) {
  std::lock_guard admission_lock(*admission_mutex_);
  return cell_admission_owner_->ReleaseTerminalApplyClaimsAfterSuccess(
      fresh_claim, fresh_success_release, validate);
}

Result<CellAdmissionClaimDTO>
ResourceAddressedExecutionKernel::AcquireApplyBringUpCellAdmissionClaim(
    const CellAdmissionClaimIntentDTO& intent, const ApplyBringUpAdmissionOfferDTO& offer) {
  std::lock_guard submit_lock(submission_mutex_);
  std::lock_guard admission_lock(*admission_mutex_);
  return cell_admission_owner_->AcquireApplyBringUpClaim(
      intent, [&](const auto& current, const auto& readiness, const auto& actual_manager_epoch) {
        if (offer.admission.echoed_query.manager_runtime_epoch != actual_manager_epoch) {
          return reject<void>("Apply bring-up Manager epoch is not current",
                              "apply_bring_up_manager_epoch_not_current");
        }
        return validate_apply_bring_up_current_locked(current, offer, readiness);
      });
}

Result<CellAdmissionClaimDTO>
ResourceAddressedExecutionKernel::AcquireSelectedComponentBringUpClaim(
    const CellAdmissionClaimIntentDTO& intent, const SelectedComponentBringUpOfferDTO& offer) {
  if (auto valid = validate_selected_bring_up_offer_identity(intent, offer); !valid.ok())
    return Result<CellAdmissionClaimDTO>::Err(valid.error());
  std::lock_guard submit_lock(submission_mutex_);
  std::lock_guard admission_lock(*admission_mutex_);
  return cell_admission_owner_->AcquireSelectedBringUpClaim(intent, [&](const auto&, const auto&) {
    return validate_selected_bring_up_predecessor_locked(intent, offer);
  });
}
Result<AuthoringAdmissionDTO> ResourceAddressedExecutionKernel::ReleaseCellAdmissionClaim(
    const CellAdmissionClaimReleaseDTO& release) {
  std::lock_guard admission_lock(*admission_mutex_);
  auto released = cell_admission_owner_->ReleaseCellAdmissionClaim(release);
  if (released.ok()) {
    refresh_execution_claims();
  }
  return released;
}

Result<AuthoringAdmissionDTO> ResourceAddressedExecutionKernel::ReconcileCellAdmissionClaims(
    const CellAdmissionReconcileDTO& reconcile) {
  std::lock_guard admission_lock(*admission_mutex_);
  auto reconciled = cell_admission_owner_->ReconcileCellAdmissionClaims(reconcile);
  if (reconciled.ok()) {
    refresh_execution_claims();
  }
  return reconciled;
}

void ResourceAddressedExecutionKernel::refresh_execution_claims() {
  // Read actual remaining authority, not a pre-reconciliation request or a
  // replayed receipt. Do not acquire the inner owner lock under state_mutex_.
  const auto claim = cell_admission_owner_->held_claim(CellAdmissionClaimKindDTO::kProgramCycle);
  auto manual = cell_admission_owner_->held_claim(CellAdmissionClaimKindDTO::kManualOperation);
  std::optional<ProgramCycleClaimRun> retired;
  std::optional<ProgramRunStopFence> retired_fence;
  std::lock_guard state_lock(state_mutex_);
  manual_operation_claim_.swap(manual);
  if (!claim.has_value()) {
    retired = std::move(program_cycle_claim_run_);
    program_cycle_claim_run_.reset();
  } else if (!program_cycle_claim_run_.has_value() ||
             program_cycle_claim_run_->claim_generation != claim->claim_generation) {
    retired = std::move(program_cycle_claim_run_);
    program_cycle_claim_run_ = ProgramCycleClaimRun{
        ProgramRunId(claim->intent.owner_transaction_id), claim->claim_generation,
        claim->intent.owner_epoch, claim->intent.program_cycle_evidence->applied_program_revision};
  }
  // An exact still-held claim retains its immutable capture and all holds.
  if (retired.has_value()) {
    const auto fence = std::find_if(
        program_run_stop_fences_.begin(), program_run_stop_fences_.end(),
        [&](const auto& row) { return row.program_run_id == retired->program_run_id; });
    if (fence != program_run_stop_fences_.end()) {
      retired_fence = std::move(*fence);
      program_run_stop_fences_.erase(fence);
    }
    std::erase_if(stop_replay_ledger_, [&](const auto& row) {
      const auto* selection = std::get_if<ProgramExecutionSelectionRequestDTO>(&row.intent);
      return selection != nullptr && selection->program_run_id == retired->program_run_id;
    });
  }
  // retired is destroyed after state_lock, outside Stop's critical section.
}

Result<std::optional<CellAdmissionSettlementEvidenceDTO>>
ResourceAddressedExecutionKernel::manual_operation_settlement_locked(
    const CellAdmissionClaimDTO& claim) const {
  using Evidence = std::optional<CellAdmissionSettlementEvidenceDTO>;
  const bool predecessor = claim.intent.owner_epoch != config_.request_owner_epoch;
  const OperationId operation_id(claim.intent.owner_transaction_id);
  if (pending_program_run_admission_.has_value() || active_operations_.contains(operation_id) ||
      (predecessor && motion_admission_unreconciled_)) {
    return Result<Evidence>::Ok(std::nullopt);
  }
  const auto slots = resource_slots_->snapshot();
  for (const auto& resource : claim.intent.scope.resources) {
    const auto slot = std::find_if(slots.begin(), slots.end(),
                                   [&](const auto& row) { return row.resource_id == resource; });
    if (slot == slots.end() || slot->state != ResourceSlotState::kFree) {
      return Result<Evidence>::Ok(std::nullopt);
    }
  }
  const auto operation =
      std::find_if(historical_operations_.begin(), historical_operations_.end(),
                   [&](const auto& row) { return row.operation_id == operation_id; });
  const bool no_effect = operation == historical_operations_.end();
  // Absence is proof only within this still-live Kernel incarnation. Its
  // admission guard runs after the pending Execute drains, and a held claim
  // protects any accepted operation from diagnostic-history eviction.
  // A successor never infers no effect from a cache miss. Its complete orphan
  // inventory must first pass the existing fresh native settlement checks.
  const auto kind = predecessor ? CellAdmissionSettlementKindDTO::kReconciled
                    : no_effect ? CellAdmissionSettlementKindDTO::kProvenNoEffect
                                : CellAdmissionSettlementKindDTO::kTerminalEffectsSettled;
  using content_identity::CanonicalValue;
  const auto digest = content_identity::sha256_digest(
      content_identity::canonical_json(CanonicalValue(CanonicalValue::Object{
          {"schema", CanonicalValue("manyforge.manual_operation_settlement.v1")},
          {"operation_id", CanonicalValue(operation_id.value())},
          {"claim_generation", CanonicalValue(std::to_string(claim.claim_generation.value()))},
          {"settlement", CanonicalValue(std::string(to_string(kind)))},
          {"operation_revision",
           CanonicalValue(no_effect ? std::string{}
                                    : std::to_string(operation->operation_revision.value()))},
          {"capture_order",
           CanonicalValue(no_effect ? std::string{}
                                    : std::to_string(operation->journal_capture_order))},
      })));
  return Result<Evidence>::Ok(CellAdmissionSettlementEvidenceDTO{kind, ContentDigest(digest)});
}

Result<void> ResourceAddressedExecutionKernel::validate_manual_operation_settlement(
    const CellAdmissionClaimDTO& claim, const CellAdmissionClaimReleaseDTO& release) const {
  std::lock_guard state_lock(state_mutex_);
  if (!manual_operation_claim_.has_value() || *manual_operation_claim_ != claim ||
      claim.intent.owner_epoch != config_.request_owner_epoch) {
    return reject<void>("manual claim differs from current Kernel authority",
                        "manual_operation_claim_stale");
  }
  const auto evidence = manual_operation_settlement_locked(claim);
  if (!evidence.ok()) return Result<void>::Err(evidence.error());
  if (!evidence.value().has_value() || *evidence.value() != release.settlement) {
    return reject<void>("manual operation has not retired with exact settlement",
                        "manual_operation_settlement_not_proven");
  }
  return Result<void>::Ok();
}

Result<CellAdmissionReconcileValidationDTO>
ResourceAddressedExecutionKernel::validate_manual_owner_reconciliation(
    const std::vector<CellAdmissionClaimDTO>& held,
    const std::vector<CellAdmissionCurrentOwnerEpochDTO>& current,
    const CellAdmissionReconcileDTO& request) const {
  constexpr auto role = CellAdmissionOwnerRoleDTO::kManualOperationOwner;
  auto expected = current;
  const auto owner = std::find_if(expected.begin(), expected.end(),
                                  [](const auto& row) { return row.owner_role == role; });
  if (owner == expected.end() || owner->owner_epoch == config_.request_owner_epoch)
    return reject<CellAdmissionReconcileValidationDTO>("manual predecessor owner is not exact",
                                                       "manual_owner_reconciliation_not_exact");
  owner->owner_epoch = config_.request_owner_epoch;
  const auto digest =
      CellAdmissionOwnerEpochReconciliationEvidenceDigest(role, config_.request_owner_epoch);
  if (!digest.ok() || request.kernel_owner_epoch != config_.request_owner_epoch ||
      request.current_owner_epochs != expected || request.retained_claims != held ||
      request.reconciliation_evidence_digest != digest.value())
    return reject<CellAdmissionReconcileValidationDTO>(
        "manual reconciliation changes unrelated authority",
        "manual_owner_reconciliation_not_exact");
  CellAdmissionReconcileValidationDTO validation{std::move(expected), {}};
  std::lock_guard state_lock(state_mutex_);
  for (const auto& claim : held) {
    if (claim.intent.owner_role != role) continue;
    if (claim.intent.claim_kind != CellAdmissionClaimKindDTO::kManualOperation ||
        !manual_operation_claim_.has_value() || *manual_operation_claim_ != claim)
      return reject<CellAdmissionReconcileValidationDTO>(
          "manual claim differs from Kernel authority", "manual_operation_claim_stale");
    const auto settlement = manual_operation_settlement_locked(claim);
    if (!settlement.ok())
      return Result<CellAdmissionReconcileValidationDTO>::Err(settlement.error());
    if (!settlement.value().has_value() ||
        settlement.value()->settlement_kind != CellAdmissionSettlementKindDTO::kReconciled)
      return reject<CellAdmissionReconcileValidationDTO>("predecessor resources are not reconciled",
                                                         "manual_operation_settlement_not_proven");
    validation.settlements.push_back({claim, role, config_.request_owner_epoch,
                                      CellAdmissionSettlementKindDTO::kReconciled, digest.value()});
  }
  return Result<CellAdmissionReconcileValidationDTO>::Ok(std::move(validation));
}

Result<void> ResourceAddressedExecutionKernel::reconcile_manual_operation_admission() {
  std::unique_lock submission_lock(submission_mutex_, std::try_to_lock);
  if (!submission_lock.owns_lock()) return Result<void>::Ok();
  std::optional<OperationStateDTO> terminal;
  {
    std::lock_guard state_lock(state_mutex_);
    if (manual_operation_claim_.has_value()) {
      const auto* operation = find_active_operation_locked(
          OperationId(manual_operation_claim_->intent.owner_transaction_id));
      if (operation != nullptr &&
          (operation->state.state == OperationStateKind::kCompleted ||
           operation->state.state == OperationStateKind::kStopped ||
           operation->state.state == OperationStateKind::kCancelledPreDispatch)) {
        // This is only a candidate. Clean-terminal eligibility is decided
        // once, inside apply_control's operation commit/state guard.
        terminal = operation->state;
      }
    }
  }
  if (terminal.has_value()) {
    const auto request_id = content_identity::sha256_digest(
        config_.request_owner_epoch.value() + "\nmanual-settlement\n" +
        terminal->operation_id.value() + "\n" +
        std::to_string(terminal->operation_revision.value()));
    const OperationControlRequestDTO request{
        {RequestId(request_id), config_.request_owner_epoch, "kernel.manual_settlement",
         terminal->operation_id.value()},
        terminal->operation_id,
        terminal->operation_revision,
        "retire owner-confirmed clean terminal manual operation",
        {}};
    const auto retired = apply_control(request,
                                       terminal->state == OperationStateKind::kCompleted
                                           ? OperationControlKind::kAcknowledgeCompleted
                                           : OperationControlKind::kReset,
                                       true);
    if (!retired.ok()) {
      const auto reason = retired.error().details.find("reason");
      // The existing exact operation-control gate wins a concurrent Stop or
      // terminal transition. Reconsider only these races on the owner wake.
      if (reason != retired.error().details.end() &&
          (reason->second == "operation_revision_stale" ||
           reason->second == "operation_not_resident" ||
           reason->second == "operation_control_state_invalid" ||
           reason->second == "operation_control_busy" ||
           reason->second == "operation_effect_in_flight" ||
           reason->second == "manual_operation_not_clean_terminal"))
        return Result<void>::Ok();
      return Result<void>::Err(retired.error());
    }
  }
  return reconcile_manual_operation_admission_locked();
}

Result<void> ResourceAddressedExecutionKernel::reconcile_manual_operation_admission_locked() {
  std::lock_guard admission_lock(*admission_mutex_);
  const auto replacement = cell_admission_owner_->manual_owner_reconciliation_request();
  if (!replacement.ok()) return Result<void>::Err(replacement.error());
  if (replacement.value().has_value()) {
    const auto reconciled =
        cell_admission_owner_->ReconcileCellAdmissionClaims(*replacement.value());
    if (!reconciled.ok()) return Result<void>::Err(reconciled.error());
    refresh_execution_claims();
  }
  std::optional<CellAdmissionClaimDTO> claim;
  std::optional<CellAdmissionSettlementEvidenceDTO> evidence;
  {
    std::lock_guard state_lock(state_mutex_);
    if (!manual_operation_claim_.has_value()) return Result<void>::Ok();
    claim = manual_operation_claim_;
    auto current = manual_operation_settlement_locked(*claim);
    if (!current.ok()) return Result<void>::Err(current.error());
    evidence = std::move(current.value());
  }
  if (!evidence.has_value()) return Result<void>::Ok();
  const auto released = cell_admission_owner_->ReleaseCellAdmissionClaim(
      {claim->intent.owner_role, claim->intent.owner_epoch, claim->intent.owner_transaction_id,
       claim->intent.request_id, claim->intent.request_payload_digest,
       claim->acquired_admission_generation, claim->claim_generation, *evidence});
  if (!released.ok()) return Result<void>::Err(released.error());
  refresh_execution_claims();
  return Result<void>::Ok();
}

std::optional<CellAdmissionClaimDTO>
ResourceAddressedExecutionKernel::ReadHeldCellAdmissionClaim(
    const CellAdmissionClaimKindDTO kind) const {
  return cell_admission_owner_->held_claim(kind);
}

std::optional<CellAdmissionClaimDTO> ResourceAddressedExecutionKernel::ReadHeldCellAdmissionClaim(
    const CellAdmissionClaimKindDTO kind, const std::string& owner_transaction_id) const {
  return cell_admission_owner_->held_claim(kind, owner_transaction_id);
}

std::vector<CellAdmissionClaimDTO> ResourceAddressedExecutionKernel::ReadHeldCellAdmissionClaims(
    const CellAdmissionClaimKindDTO kind) const {
  return cell_admission_owner_->held_claims(kind);
}

Result<CellAdmissionRecoveryReceiptDTO>
ResourceAddressedExecutionKernel::GetCellAdmissionRecoveryReceipt(
    const CellAdmissionRecoveryReceiptQueryDTO& query) const {
  return cell_admission_owner_->GetCellAdmissionRecoveryReceipt(query);
}

Result<void> ResourceAddressedExecutionKernel::ValidateProgramResetSceneAuthorization(
    const ProgramResetSceneAuthorizationDTO& authorization) const {
  std::lock_guard admission_lock(*admission_mutex_);
  return cell_admission_owner_->ValidateProgramResetSceneAuthorization(authorization);
}

Result<void> ResourceAddressedExecutionKernel::Publish(
    const ManagerLivenessObservationDTO& observation) {
  // Deliberately bypasses the operation admission mutex: receiver-local
  // containment and Stop acceptance must not wait on liveness publication.
  return cell_admission_owner_->Publish(observation);
}

Result<OperatorAlarmAttentionAcceptedDTO>
ResourceAddressedExecutionKernel::PublishOperatorAlarmAttention(
    const OperatorAlarmAttentionSnapshotDTO& snapshot) {
  if (auto valid = validate_operator_alarm_attention_snapshot(snapshot); !valid.ok()) {
    return Result<OperatorAlarmAttentionAcceptedDTO>::Err(valid.error());
  }
  if (snapshot.cell_id != config_.cell_id) {
    return Result<OperatorAlarmAttentionAcceptedDTO>::Err(
        error("operator_alarm_attention_cell_mismatch",
              "operator-alarm attention projection names another cell",
              "operator_alarm_attention_cell_mismatch"));
  }
  // Single-host precondition (spec 530 §11): owner incarnations are
  // CLOCK_BOOTTIME stamps, comparable only within the Kernel's own host boot.
  // A projection from another host or boot is refused, never compared.
  if (config_.operator_alarm_attention.has_value() &&
      snapshot.owner_host_boot_id != config_.operator_alarm_attention->host_boot_id) {
    return Result<OperatorAlarmAttentionAcceptedDTO>::Err(
        error("operator_alarm_attention_host_mismatch",
              "operator-alarm attention was published from another host boot",
              "operator_alarm_attention_host_mismatch"));
  }
  // A small bounded copy under the state lock that admission reads; Stop
  // acceptance never waits on any I/O here.
  //
  // Fencing contract (spec 530 §11). Owner epochs are ordered by their
  // incarnation alone, so the Kernel keeps no list of replaced epochs: a new
  // epoch is accepted only with a strictly greater incarnation than the
  // retained one; an equal incarnation under another epoch, or a smaller one,
  // is refused. Within one epoch the incarnation is fixed, generations are
  // monotonic, and one generation names exactly one content: identical
  // content is the heartbeat that refreshes the receipt time, while different
  // content under the same generation is refused.
  std::lock_guard state_lock(state_mutex_);
  if (operator_alarm_attention_.has_value()) {
    const auto& retained = operator_alarm_attention_->snapshot;
    if (retained.owner_epoch == snapshot.owner_epoch) {
      if (snapshot.owner_host_boot_id != retained.owner_host_boot_id ||
          snapshot.owner_incarnation_us != retained.owner_incarnation_us) {
        return Result<OperatorAlarmAttentionAcceptedDTO>::Err(
            error("operator_alarm_attention_conflict",
                  "operator-alarm owner epoch was republished with another incarnation",
                  "operator_alarm_attention_owner_incarnation_conflict"));
      }
      if (snapshot.generation < retained.generation) {
        return Result<OperatorAlarmAttentionAcceptedDTO>::Err(
            error("operator_alarm_attention_stale",
                  "operator-alarm attention generation precedes the retained one",
                  "operator_alarm_attention_generation_stale"));
      }
      if (snapshot.generation == retained.generation && snapshot != retained) {
        return Result<OperatorAlarmAttentionAcceptedDTO>::Err(
            error("operator_alarm_attention_conflict",
                  "operator-alarm attention generation was republished with different content",
                  "operator_alarm_attention_generation_conflict"));
      }
    } else if (snapshot.owner_incarnation_us == retained.owner_incarnation_us) {
      return Result<OperatorAlarmAttentionAcceptedDTO>::Err(
          error("operator_alarm_attention_conflict",
                "another operator-alarm owner epoch claims the retained incarnation",
                "operator_alarm_attention_owner_incarnation_conflict"));
    } else if (snapshot.owner_incarnation_us < retained.owner_incarnation_us) {
      return Result<OperatorAlarmAttentionAcceptedDTO>::Err(
          error("operator_alarm_attention_stale",
                "operator-alarm attention names an older owner incarnation",
                "operator_alarm_attention_owner_incarnation_stale"));
    }
  }
  operator_alarm_attention_ = RetainedOperatorAlarmAttentionDTO{snapshot, clock_.now()};
  return Result<OperatorAlarmAttentionAcceptedDTO>::Ok(
      OperatorAlarmAttentionAcceptedDTO{snapshot.owner_epoch, snapshot.generation});
}

std::optional<ErrorDTO> ResourceAddressedExecutionKernel::operator_alarm_attention_refusal_locked(
    const std::vector<ResourceId>& reserved_resources) const {
  if (!config_.operator_alarm_attention.has_value()) return std::nullopt;
  std::optional<OperatorAlarmAttentionConfigurationDTO> active_configuration;
  if (stop_retained_bundle_id_.has_value() && stop_graph_revision_.has_value()) {
    active_configuration =
        OperatorAlarmAttentionConfigurationDTO{*stop_retained_bundle_id_, *stop_graph_revision_};
  }
  return evaluate_operator_alarm_motion_attention(
      operator_alarm_attention_, *config_.operator_alarm_attention, config_.cell_id,
      active_configuration, clock_.now(), reserved_resources);
}

std::vector<ResourceId> ResourceAddressedExecutionKernel::program_selection_resources_locked(
    const std::vector<std::uint32_t>& node_indices) const {
  std::set<ResourceId> resources;
  if (!program_cycle_claim_run_.has_value() || !program_cycle_claim_run_->capture.has_value() ||
      !program_cycle_claim_run_->capture->table || !program_cycle_claim_run_->capture->binding) {
    return {};
  }
  const auto& capture = *program_cycle_claim_run_->capture;
  const auto& nodes = capture.table->nodes;
  for (const auto index : node_indices) {
    if (index >= nodes.size()) continue;
    for (const auto& effect : nodes[index].target_effects) {
      if (!effect.effectful) continue;
      // The exact sealed Program Binding names each target role's resources.
      for (const auto& target : capture.binding->target_role_bindings) {
        if (target.target_role_id != effect.target_role_id) continue;
        for (const auto& role : target.resource_role_bindings) {
          resources.insert(role.resource_ids.begin(), role.resource_ids.end());
        }
      }
    }
  }
  return {resources.begin(), resources.end()};
}

Result<void> ResourceAddressedExecutionKernel::check_program_node_motion_attention(
    const ProgramNodeMotionAttentionCheckDTO& request) const {
  std::lock_guard state_lock(state_mutex_);
  if (request.request_context.request_owner_epoch != config_.request_owner_epoch ||
      !program_cycle_claim_run_.has_value() || !program_cycle_claim_run_->capture.has_value() ||
      !program_cycle_claim_run_->capture->table ||
      request.program_run_id != program_cycle_claim_run_->program_run_id ||
      request.program_runner_owner_epoch != program_cycle_claim_run_->program_runner_owner_epoch ||
      request.claim_generation != program_cycle_claim_run_->claim_generation) {
    return reject<void>("Program node check does not name the current captured owner/claim",
                        "program_selection_stale", ErrorCode::kInvalidStateTransition);
  }
  const auto& nodes = program_cycle_claim_run_->capture->table->nodes;
  const auto node = std::lower_bound(
      nodes.begin(), nodes.end(), request.node_instance_id,
      [](const auto& row, const auto& id) { return row.instance_id < id; });
  if (node == nodes.end() || node->instance_id != request.node_instance_id) {
    return reject<void>("Program node is not present in the captured Program",
                        "program_node_unknown", ErrorCode::kInvalidRequest);
  }
  if (auto refusal = operator_alarm_attention_refusal_locked(program_selection_resources_locked(
          {static_cast<std::uint32_t>(node - nodes.begin())}))) {
    return Result<void>::Err(std::move(*refusal));
  }
  return Result<void>::Ok();
}

void ResourceAddressedExecutionKernel::LatchAndContain(
    const ManagerLivenessLossIntentDTO& intent) noexcept {
  try {
    if (!cell_admission_owner_->LatchManagerLivenessLoss(intent)) {
      return;
    }
    StopGeneration expected_stop_generation(0U);
    {
      std::lock_guard state_lock(state_mutex_);
      // This is the execution-side fail-safe latch. It is not a second
      // liveness timer or state owner and is deliberately set before Stop I/O.
      manager_liveness_motion_inhibited_ = true;
      motion_admission_closed_ = true;
      expected_stop_generation = cell_stop_generation_;
    }
    const auto material = config_.request_owner_epoch.value() + "\nmanager-liveness-loss\n" +
                          intent.cell_id.value() + "\n" + intent.receiver_runtime_unit_id.value() +
                          "\n" + intent.receiver_runtime_epoch.value() + "\n" +
                          std::to_string(intent.receiver_status_sequence);
    FullStopIntentDTO containment{
        PublicRequestContextDTO{RequestId(manyforge::content_identity::sha256_digest(material)),
                                config_.request_owner_epoch, "kernel.manager_liveness",
                                intent.receiver_runtime_unit_id.value()},
        expected_stop_generation,
        RequestAttributionDTO{RequestTransportDTO::kInternal, RequestAssuranceDTO::kUnverified,
                              std::nullopt}};
    auto stopped = accept_stop(EmergencyStopProjectedIntentDTO{std::move(containment)});
    if (!stopped.ok()) {
      std::lock_guard state_lock(state_mutex_);
      manager_liveness_motion_inhibited_ = true;
      motion_admission_closed_ = true;
    } else if (stopped.value().replay_status == RequestReplayStatusDTO::kAcceptedNew &&
               stopped.value().joined_predecessor_stops.empty()) {
      // Attribute only a fresh containment Stop that joined no operator Stop:
      // its latch, if any, was caused by this availability-class loss alone.
      std::lock_guard state_lock(state_mutex_);
      attribute_containment_stop_locked(stopped.value(),
                                        StopFaultLatchOriginDTO::kProviderAvailabilityLoss);
    }
  } catch (...) {
    try {
      std::lock_guard state_lock(state_mutex_);
      manager_liveness_motion_inhibited_ = true;
      motion_admission_closed_ = true;
    } catch (...) {
    }
  }
}

void ResourceAddressedExecutionKernel::attribute_containment_stop_locked(
    const StopAcceptedDTO& stopped, const StopFaultLatchOriginDTO origin) {
  constexpr std::size_t kMaximumAttributedContainmentStops = 64U;
  if (attributed_containment_stops_.size() >= kMaximumAttributedContainmentStops) {
    attributed_containment_stops_.erase(attributed_containment_stops_.begin());
  }
  attributed_containment_stops_.push_back({stopped.stop_id, stopped.stop_generation, origin});
}

Result<ComponentIncidentContainmentReceiptDTO>
ResourceAddressedExecutionKernel::RequestComponentIncidentContainment(
    const ComponentIncidentContainmentIntentDTO& intent) {
  if (!intent.locally_valid() || !content_identity::is_utf8_nfc(intent.incident_id)) {
    return reject<ComponentIncidentContainmentReceiptDTO>(
        "component incident containment request is invalid", "incident_containment_invalid",
        ErrorCode::kInvalidRequest);
  }
  StopGeneration expected_stop_generation(0U);
  std::optional<ProgramRunId> program_run_id;
  {
    std::lock_guard state_lock(state_mutex_);
    // Closed before any Stop I/O, exactly like the liveness fail-safe.
    motion_admission_closed_ = true;
    for (const auto& remembered : incident_containments_) {
      if (remembered.incident_id == intent.incident_id) {
        return Result<ComponentIncidentContainmentReceiptDTO>::Ok(remembered.receipt);
      }
    }
    expected_stop_generation = cell_stop_generation_;
    if (program_cycle_claim_run_.has_value()) {
      program_run_id = program_cycle_claim_run_->program_run_id;
    }
  }
  // The request identity is this Kernel's own: a repeated request for the
  // same incident replays the same Stop instead of issuing another.
  const auto material = config_.request_owner_epoch.value() + "\ncomponent-incident\n" +
                        config_.cell_id.value() + "\n" + intent.incident_id;
  FullStopIntentDTO containment{
      PublicRequestContextDTO{RequestId(manyforge::content_identity::sha256_digest(material)),
                              config_.request_owner_epoch, "cclc.component_incident",
                              intent.incident_id},
      expected_stop_generation,
      RequestAttributionDTO{RequestTransportDTO::kInternal, RequestAssuranceDTO::kUnverified,
                            std::nullopt}};
  auto stopped = accept_stop(EmergencyStopProjectedIntentDTO{std::move(containment)}, nullptr,
                             nullptr, StopFaultTriggerDTO::kPhysicalRestUnproven);
  if (!stopped.ok()) {
    return Result<ComponentIncidentContainmentReceiptDTO>::Err(stopped.error());
  }
  ComponentIncidentContainmentReceiptDTO receipt{stopped.value().stop_id,
                                                 stopped.value().stop_generation,
                                                 {},
                                                 std::move(program_run_id)};
  receipt.affected_operation_ids.reserve(stopped.value().affected_operation_revisions.size());
  for (const auto& affected : stopped.value().affected_operation_revisions) {
    receipt.affected_operation_ids.push_back(affected.operation_id);
  }
  {
    std::lock_guard state_lock(state_mutex_);
    if (stopped.value().replay_status == RequestReplayStatusDTO::kAcceptedNew &&
        stopped.value().joined_predecessor_stops.empty()) {
      attribute_containment_stop_locked(stopped.value(),
                                        StopFaultLatchOriginDTO::kComponentIncident);
    }
    constexpr std::size_t kMaximumRememberedIncidentContainments = 64U;
    if (incident_containments_.size() >= kMaximumRememberedIncidentContainments) {
      incident_containments_.erase(incident_containments_.begin());
    }
    incident_containments_.push_back(
        {intent.incident_id, receipt,
         stopped.value().replay_status == RequestReplayStatusDTO::kAcceptedNew
             ? std::optional(intent.controllable_component_id)
             : std::nullopt});
  }
  return Result<ComponentIncidentContainmentReceiptDTO>::Ok(std::move(receipt));
}

Result<AuthoringAdmissionDTO> ResourceAddressedExecutionKernel::ReplaceManagerLivenessRequiredSet(
    const ManagerLivenessRequiredSetDTO& required_set) {
  return cell_admission_owner_->ReplaceManagerLivenessRequiredSet(required_set);
}

}  // namespace manyforge::core
