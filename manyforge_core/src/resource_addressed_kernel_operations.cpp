#include "resource_addressed_execution_kernel_internal.hpp"
#include "program_stop_projection_internal.hpp"

#include <atomic>

namespace manyforge::core {
namespace {

/// OD-72: the process-wide order of accepted speed values. Allocated under
/// the Kernel state lock together with the value it orders (a setting write,
/// an operation's scale, a motion's seed), so a later allocation is always
/// the later value; process-wide because a retained participant outlives a
/// Kernel. Starts at 1: 0 is "unordered".
[[nodiscard]] std::uint64_t next_runtime_scale_value_sequence() noexcept {
  static std::atomic<std::uint64_t> last{0U};
  return last.fetch_add(1U, std::memory_order_relaxed) + 1U;
}

}  // namespace

Result<ResourceAddressedExecutionKernel::RuntimeScaleControlResolution>
ResourceAddressedExecutionKernel::resolve_runtime_scale_control_target_locked(
    const RuntimeScaleControlSelectorDTO& selector) const {
  if (selector.program_binding_id.empty() || selector.component_id.empty() ||
      selector.command_target_id.empty()) {
    return reject<RuntimeScaleControlResolution>("runtime-scale control selector is incomplete",
                                                 "runtime_scale_control_selector_invalid",
                                                 ErrorCode::kInvalidRequest);
  }

  std::optional<RuntimeScaleControlResolution> selected;
  bool target_found = false;
  bool not_listening = false;
  // A lane that cannot scale is an unqualified route; a capable lane whose
  // controller is not listening right now refuses with its own reason.
  const auto mechanism_skips = [&](const RuntimeParticipantExecutionRegistration& registration) {
    const auto availability = runtime_scale_availability_of(registration.engine);
    if (availability == RuntimeScaleMechanismAvailability::kControllerNotListening)
      not_listening = true;
    return availability != RuntimeScaleMechanismAvailability::kAvailable;
  };
  for (const auto& registration : registrations_) {
    const auto& binding = registration.binding.value();
    if (binding.program_binding_id != selector.program_binding_id ||
        binding.controllable_component_id != selector.component_id ||
        binding.command_target_id != selector.command_target_id) {
      continue;
    }
    target_found = true;
    // The persistent operator setting belongs to the motion binding. A
    // qualified operation-scale capability authorizes a command to an already
    // addressed operation; it does not introduce another setting/authority.
    if (binding.capability_id.value() != "execution.trajectory.execute" ||
        !binding.control_path.has_value() ||
        !binding.control_path->runtime_scale_qualification.has_value() ||
        mechanism_skips(registration)) {
      continue;
    }
    const auto& control = *binding.control_path;
    const auto& qualification = *control.runtime_scale_qualification;
    RuntimeScaleControlResolution candidate{selector,
                                            control.control_domain_id,
                                            control.control_domain_revision,
                                            control.control_binding_id,
                                            control.control_binding_revision,
                                            qualification.scaling_domain_id,
                                            control.active_control_configuration_id,
                                            control.active_control_configuration_generation,
                                            qualification.coordinated_barrier_id};
    if (selected.has_value() && *selected != candidate) {
      return reject<RuntimeScaleControlResolution>(
          "runtime-scale target resolves to more than one control scope",
          "runtime_scale_control_scope_ambiguous", ErrorCode::kInvalidStateTransition);
    }
    selected = std::move(candidate);
  }
  // Managed runtimes retain physical registrations independently of any
  // authored Program. Resolve a captured Program role through its immutable
  // binding to that same physical target instead of installing aliases for
  // every saved Program in the execution host.
  if (!selected.has_value() && program_cycle_claim_run_ && program_cycle_claim_run_->capture &&
      program_cycle_claim_run_->capture->binding &&
      program_cycle_claim_run_->capture->program_binding_id == selector.program_binding_id) {
    const auto& capture = *program_cycle_claim_run_->capture;
    const auto target =
        std::find_if(capture.binding->target_bindings.begin(),
                     capture.binding->target_bindings.end(), [&](const auto& binding) {
                       return binding.target.component_id == selector.component_id &&
                              binding.target.command_target_id == selector.command_target_id;
                     });
    if (target != capture.binding->target_bindings.end()) {
      target_found = true;
      for (const auto& registration : registrations_) {
        const auto& binding = registration.binding.value();
        if (binding.controllable_component_id != target->target.component_id ||
            binding.command_target_id != target->target.command_target_id ||
            binding.capability_id.value() != "execution.trajectory.execute" ||
            !binding.control_path || !binding.control_path->runtime_scale_qualification ||
            mechanism_skips(registration))
          continue;
        const auto& control = *binding.control_path;
        const auto& qualification = *control.runtime_scale_qualification;
        RuntimeScaleControlResolution candidate{selector,
                                                control.control_domain_id,
                                                control.control_domain_revision,
                                                control.control_binding_id,
                                                control.control_binding_revision,
                                                qualification.scaling_domain_id,
                                                control.active_control_configuration_id,
                                                control.active_control_configuration_generation,
                                                qualification.coordinated_barrier_id};
        if (selected && *selected != candidate) {
          return reject<RuntimeScaleControlResolution>(
              "captured runtime-scale target resolves to more than one control scope",
              "runtime_scale_control_scope_ambiguous", ErrorCode::kInvalidStateTransition);
        }
        selected = std::move(candidate);
      }
    }
  }
  if (!selected.has_value() && not_listening) {
    return reject<RuntimeScaleControlResolution>(
        "the runtime-scale target's controller is not listening for scale input",
        kRuntimeScaleControllerNotListening, ErrorCode::kUnsupportedCapability);
  }
  if (!selected.has_value()) {
    return reject<RuntimeScaleControlResolution>(
        target_found ? "runtime-scale target has no qualified control binding"
                     : "runtime-scale target is absent from the current Program Binding",
        target_found ? "runtime_scale_control_unsupported" : "runtime_scale_control_target_missing",
        target_found ? ErrorCode::kUnsupportedCapability : ErrorCode::kMissingReference);
  }
  return Result<RuntimeScaleControlResolution>::Ok(std::move(*selected));
}

Result<ResourceAddressedExecutionKernel::RuntimeScaleControlScope>
ResourceAddressedExecutionKernel::resolve_runtime_scale_control_locked(
    const RuntimeScaleControlSelectorDTO& selector) const {
  RuntimeScaleControlScope scope{selector, {}};
  if (selector.group_targets.empty()) {
    auto member = resolve_runtime_scale_control_target_locked(selector);
    if (!member.ok()) return Result<RuntimeScaleControlScope>::Err(member.error());
    scope.members.push_back(std::move(member.value()));
  } else {
    if (!selector.component_id.empty() || !selector.command_target_id.empty() ||
        selector.group_targets.size() < 2U ||
        selector.group_targets.size() > kMaxOperationParticipants) {
      return reject<RuntimeScaleControlScope>(
          "group velocity requires one complete bounded target set",
          "runtime_scale_control_selector_invalid", ErrorCode::kInvalidRequest);
    }
    scope.members.reserve(selector.group_targets.size());
    for (const auto& target : selector.group_targets) {
      auto member = resolve_runtime_scale_control_target_locked(
          {selector.program_binding_id, target.component_id, target.command_target_id});
      if (!member.ok()) return Result<RuntimeScaleControlScope>::Err(member.error());
      scope.members.push_back(std::move(member.value()));
    }
    std::sort(scope.members.begin(), scope.members.end(),
              [](const auto& a, const auto& b) { return a.key() < b.key(); });
    if (std::adjacent_find(scope.members.begin(), scope.members.end(),
                           [](const auto& a, const auto& b) { return a.same_control_scope(b); }) !=
        scope.members.end()) {
      return reject<RuntimeScaleControlScope>("group velocity targets alias the same control scope",
                                              "runtime_scale_group_member_duplicate",
                                              ErrorCode::kInvalidRequest);
    }
  }
  return Result<RuntimeScaleControlScope>::Ok(std::move(scope));
}

double ResourceAddressedExecutionKernel::runtime_scale_setting_for_binding_locked(
    const RuntimeParticipantBindingDTO& binding) const noexcept {
  if (!binding.control_path.has_value() ||
      !binding.control_path->runtime_scale_qualification.has_value()) {
    return 1.0;
  }
  const auto& control = *binding.control_path;
  const auto& qualification = *control.runtime_scale_qualification;
  const RuntimeScaleControlResolution resolution{
      RuntimeScaleControlSelectorDTO{binding.program_binding_id, binding.controllable_component_id,
                                     binding.command_target_id},
      control.control_domain_id,
      control.control_domain_revision,
      control.control_binding_id,
      control.control_binding_revision,
      qualification.scaling_domain_id,
      control.active_control_configuration_id,
      control.active_control_configuration_generation};
  const auto found =
      std::find_if(runtime_scale_control_settings_.begin(), runtime_scale_control_settings_.end(),
                   [&](const RuntimeScaleControlSetting& setting) {
                     return setting.resolution.members.size() == 1U &&
                            setting.resolution.members.front().same_control_scope(resolution);
                   });
  return found == runtime_scale_control_settings_.end() ? 1.0 : found->requested_scale;
}

Result<RuntimeScaleControlStatusDTO> ResourceAddressedExecutionKernel::get_runtime_scale_control(
    const RuntimeScaleControlSelectorDTO& selector) const {
  std::lock_guard state_lock(state_mutex_);
  return runtime_scale_control_locked(selector, false);
}

Result<RuntimeScaleControlStatusDTO>
ResourceAddressedExecutionKernel::present_runtime_scale_control(
    const RuntimeScaleControlSelectorDTO& selector) const {
  std::lock_guard state_lock(state_mutex_);
  return runtime_scale_control_locked(selector, true);
}

Result<RuntimeScaleControlStatusDTO> ResourceAddressedExecutionKernel::runtime_scale_control_locked(
    const RuntimeScaleControlSelectorDTO& selector, const bool present_group) const {
  const auto resolved = resolve_runtime_scale_control_locked(selector);
  if (!resolved.ok()) {
    return Result<RuntimeScaleControlStatusDTO>::Ok(
        RuntimeScaleControlStatusDTO{selector, RuntimeScaleControlGeneration(0U), 1.0, false, true,
                                     std::nullopt,  // control_domain_id
                                     std::nullopt,  // control_domain_revision
                                     std::nullopt,  // control_binding_id
                                     std::nullopt,  // control_binding_revision
                                     std::nullopt,  // scaling_domain_id
                                     std::nullopt,  // active_control_configuration_id
                                     std::nullopt,  // active_control_configuration_generation
                                     std::nullopt,  // active_operation_id
                                     std::nullopt,  // active_operation_revision
                                     std::nullopt,  // active_operation_scale_generation
                                     std::nullopt,  // effective_scale
                                     std::nullopt,  // application_state
                                     resolved.error()});
  }
  const auto& scope = resolved.value();
  const bool group = scope.members.size() > 1U;
  const auto setting =
      std::find_if(runtime_scale_control_settings_.begin(), runtime_scale_control_settings_.end(),
                   [&](const RuntimeScaleControlSetting& candidate) {
                     return candidate.resolution.same_control_scope(scope);
                   });
  const auto generation = setting == runtime_scale_control_settings_.end()
                              ? RuntimeScaleControlGeneration(0U)
                              : setting->generation;
  const auto requested_scale =
      setting == runtime_scale_control_settings_.end() ? 1.0 : setting->requested_scale;
  const auto member_effective_scale = [&](const RuntimeScaleControlResolution& member) {
    const auto effective = std::find_if(
        runtime_scale_control_settings_.begin(), runtime_scale_control_settings_.end(),
        [&](const RuntimeScaleControlSetting& candidate) {
          return candidate.resolution.members.size() == 1U &&
                 candidate.resolution.members.front().same_control_scope(member);
        });
    return effective == runtime_scale_control_settings_.end() ? 1.0
                                                              : effective->requested_scale;
  };

  const auto matches_scope = [&](const RuntimeParticipantBindingDTO& binding) {
    if (!binding.control_path || !binding.control_path->runtime_scale_qualification) return false;
    const auto& control = *binding.control_path;
    const auto& qualification = *control.runtime_scale_qualification;
    return std::any_of(scope.members.begin(), scope.members.end(), [&](const auto& member) {
      return control.control_domain_id == member.control_domain_id &&
             control.control_domain_revision == member.control_domain_revision &&
             control.control_binding_id == member.control_binding_id &&
             control.control_binding_revision == member.control_binding_revision &&
             qualification.scaling_domain_id == member.scaling_domain_id &&
             control.active_control_configuration_id == member.active_control_configuration_id &&
             control.active_control_configuration_generation ==
                 member.active_control_configuration_generation;
    });
  };
  std::vector<const ActiveOperation*> active;
  for (const auto& [operation_id, operation] : active_operations_) {
    static_cast<void>(operation_id);
    if (terminal(operation->state.state)) {
      continue;
    }
    bool matches = false;
    for (const auto& participant : operation->participants) {
      const auto& binding = participant.binding.value();
      if (participant.payload_kind != OperationParticipantPayloadKind::kMotion ||
          !binding.control_path.has_value() ||
          !binding.control_path->runtime_scale_qualification.has_value()) {
        continue;
      }
      matches = matches_scope(binding);
      if (matches) {
        break;
      }
    }
    if (!matches) {
      continue;
    }
    active.push_back(operation.get());
  }

  if (present_group && !group && active.size() == 1U &&
      active.front()->intent.coordination_policy == OperationCoordinationPolicy::kCoordinatedAll &&
      active.front()->participants.size() > 1U &&
      std::all_of(active.front()->participants.begin(), active.front()->participants.end(),
                  [](const auto& row) {
                    return row.payload_kind == OperationParticipantPayloadKind::kMotion;
                  })) {
    RuntimeScaleControlSelectorDTO group_selector{active.front()->intent.program_binding_id,
                                                  ControllableComponentId(""), CommandTargetId("")};
    for (const auto& participant : active.front()->participants) {
      const auto& binding = participant.binding.value();
      group_selector.group_targets.push_back({binding.controllable_component_id, binding.command_target_id});
    }
    return runtime_scale_control_locked(group_selector, false);
  }

  // An ordinary execution group consists of independent operations. Present
  // its one authoritative velocity setting when the selected target belongs
  // to exactly one current multi-motion group. The target set is derived from
  // the sealed Program capture; the browser never invents group membership.
  if (present_group && !group && program_cycle_claim_run_ && program_cycle_claim_run_->capture &&
      !active.empty()) {
    const auto& capture = *program_cycle_claim_run_->capture;
    std::optional<std::uint32_t> selected_node;
    for (const auto* operation : active) {
      for (std::size_t participant_index = 0;
           participant_index < operation->participants.size() &&
           participant_index < operation->intent.participants.size();
           ++participant_index) {
        const auto& participant = operation->participants[participant_index];
        if (participant.binding.value().controllable_component_id != selector.component_id ||
            participant.binding.value().command_target_id != selector.command_target_id)
          continue;
        const auto& execution_scope =
            operation->intent.participants[participant_index].execution_scope;
        if (!execution_scope) continue;
        const auto node =
            std::lower_bound(capture.table->nodes.begin(), capture.table->nodes.end(),
                             execution_scope->node_instance_id,
                             [](const auto& row, const auto& id) { return row.instance_id < id; });
        if (node != capture.table->nodes.end() &&
            node->instance_id == execution_scope->node_instance_id) {
          selected_node = static_cast<std::uint32_t>(node - capture.table->nodes.begin());
          break;
        }
      }
      if (selected_node) break;
    }
    const ProgramExecutionSelectionDTO* selected_group = nullptr;
    if (selected_node) {
      for (const auto& candidate : capture.table->named_selections) {
        if (!std::binary_search(candidate.node_indices.begin(), candidate.node_indices.end(),
                                *selected_node))
          continue;
        if (selected_group != nullptr) {
          selected_group = nullptr;
          break;
        }
        selected_group = &candidate;
      }
    }
    if (selected_group != nullptr) {
      RuntimeScaleControlSelectorDTO group_selector{capture.program_binding_id, ControllableComponentId(""),
                                                    CommandTargetId("")};
      for (const auto node_index : selected_group->node_indices) {
        const auto& node = capture.table->nodes[node_index];
        for (const auto& effect : node.target_effects) {
          if (!effect.effectful ||
              effect.capability_id != CapabilityId("execution.trajectory.execute"))
            continue;
          const auto target = std::find_if(
              capture.binding->target_bindings.begin(),
              capture.binding->target_bindings.end(),
              [&](const auto& binding) { return binding.target == effect.target; });
          if (target == capture.binding->target_bindings.end()) continue;
          group_selector.group_targets.push_back(
              {target->target.component_id, target->target.command_target_id});
        }
      }
      std::sort(group_selector.group_targets.begin(), group_selector.group_targets.end(),
                [](const auto& left, const auto& right) {
                  return std::tie(left.component_id, left.command_target_id) <
                         std::tie(right.component_id, right.command_target_id);
                });
      group_selector.group_targets.erase(
          std::unique(group_selector.group_targets.begin(), group_selector.group_targets.end()),
          group_selector.group_targets.end());
      if (group_selector.group_targets.size() > 1U)
        return runtime_scale_control_locked(group_selector, false);
    }
  }

  const auto& first = scope.members.front();
  RuntimeScaleControlStatusDTO status{selector,
                                      generation,
                                      requested_scale,
                                      true,
                                      false,
                                      first.control_domain_id,
                                      first.control_domain_revision,
                                      first.control_binding_id,
                                      first.control_binding_revision,
                                      first.scaling_domain_id,
                                      first.active_control_configuration_id,
                                      first.active_control_configuration_generation,
                                      std::nullopt,
                                      std::nullopt,
                                      std::nullopt,
                                      std::nullopt,
                                      std::nullopt,
                                      std::nullopt};
  if (group) {
    status.control_domain_id.reset();
    status.control_domain_revision.reset();
    status.control_binding_id.reset();
    status.control_binding_revision.reset();
    status.scaling_domain_id.reset();
    status.active_control_configuration_id.reset();
    status.active_control_configuration_generation.reset();
    for (const auto& member : scope.members) {
      status.group_members.push_back(
          {{member.selector.component_id, member.selector.command_target_id},
           member.control_domain_id,
           member.control_domain_revision,
           member.control_binding_id,
           member.control_binding_revision,
           member.scaling_domain_id,
           member.active_control_configuration_id,
           member.active_control_configuration_generation});
    }
    const auto first_effective = member_effective_scale(scope.members.front());
    if (std::all_of(scope.members.begin(), scope.members.end(), [&](const auto& member) {
          return std::abs(member_effective_scale(member) - first_effective) <= 1e-12;
        })) {
      status.effective_scale = first_effective;
    } else {
      // A null effective value with an applied state explicitly means that
      // the member settings are mixed, not that evidence is unavailable.
      status.application_state = RuntimeScaleApplicationState::kApplied;
    }
  }
  if (!group && active.size() > 1U) {
    status.frozen = true;
    status.reason = error(std::string(to_string(ErrorCode::kInvalidStateTransition)),
                          "more than one active motion matches the runtime-scale control scope",
                          "runtime_scale_control_active_ambiguous");
    return Result<RuntimeScaleControlStatusDTO>::Ok(std::move(status));
  }
  if (active.size() == 1U) {
    status.active_operation_id = active.front()->state.operation_id;
    status.active_operation_revision = active.front()->state.operation_revision;
    if (active.front()->runtime_scale_status.has_value()) {
      status.active_operation_scale_generation =
          active.front()->runtime_scale_status->runtime_scale_generation;
      status.effective_scale = active.front()->runtime_scale_status->effective_scale;
      status.application_state = active.front()->runtime_scale_status->state;
    }
  } else if (group && !active.empty()) {
    const auto first_scale =
        active.front()->runtime_scale_status
            ? std::optional(active.front()->runtime_scale_status->effective_scale)
            : std::nullopt;
    if (first_scale && std::all_of(active.begin(), active.end(), [&](const auto* operation) {
          return operation->runtime_scale_status &&
                 std::abs(operation->runtime_scale_status->effective_scale - *first_scale) <= 1e-12;
        }))
      status.effective_scale = *first_scale;
    else
      status.effective_scale.reset();
    if (std::any_of(active.begin(), active.end(), [](const auto* operation) {
          return operation->runtime_scale_status &&
                 operation->runtime_scale_status->state == RuntimeScaleApplicationState::kFailed;
        }))
      status.application_state = RuntimeScaleApplicationState::kFailed;
    else if (std::any_of(active.begin(), active.end(), [](const auto* operation) {
               return operation->runtime_scale_status &&
                      operation->runtime_scale_status->state ==
                          RuntimeScaleApplicationState::kRejected;
             }))
      status.application_state = RuntimeScaleApplicationState::kRejected;
    else if (std::any_of(active.begin(), active.end(), [](const auto* operation) {
               return operation->runtime_scale_status && operation->runtime_scale_status->state ==
                                                             RuntimeScaleApplicationState::kPending;
             }))
      status.application_state = RuntimeScaleApplicationState::kPending;
    else
      status.application_state = RuntimeScaleApplicationState::kApplied;
  }
  // Retain the setting and live observation while the exact route is closed.
  // A retained scaling capability is not evidence of current route readiness.
  const bool route_closed =
      std::any_of(scope.members.begin(), scope.members.end(), [&](const auto& member) {
        const auto route = runtime_route_configuration_readbacks_.find(member.control_domain_id);
        return runtime_route_configuration_publication_required_.contains(
                   member.control_domain_id) ||
               (route != runtime_route_configuration_readbacks_.end() &&
                (!route->second.operation_admission_open ||
                 route->second.active_configuration.active_control_configuration_id !=
                     member.active_control_configuration_id ||
                 route->second.active_configuration.active_control_configuration_generation !=
                     member.active_control_configuration_generation));
      });
  if (route_closed) {
    status.frozen = true;
    status.reason = error(std::string(to_string(ErrorCode::kInvalidStateTransition)),
                          "runtime-scale control requires a current reconciled route",
                          "runtime_route_configuration_not_ready");
    return Result<RuntimeScaleControlStatusDTO>::Ok(std::move(status));
  }
  if (active.empty()) {
    return Result<RuntimeScaleControlStatusDTO>::Ok(std::move(status));
  }
  if (std::any_of(active.begin(), active.end(), [](const auto* operation) {
        return operation->state.state == OperationStateKind::kStopping ||
               operation->state.active_stop_id.has_value();
      })) {
    status.frozen = true;
    status.reason = error(std::string(to_string(ErrorCode::kInvalidStateTransition)),
                          "runtime-scale control is frozen while Stop is active",
                          "runtime_scale_control_stop_active");
  } else if (!group &&
             active.front()->intent.coordination_policy ==
                 OperationCoordinationPolicy::kCoordinatedAll &&
             active.front()->participants.size() > 1U) {
    status.frozen = true;
    status.reason = error(std::string(to_string(ErrorCode::kInvalidStateTransition)),
                          "synchronized motion requires its group velocity control",
                          "runtime_scale_group_control_required");
  } else if (group && std::any_of(active.begin(), active.end(), [&](const auto* operation) {
               return std::any_of(operation->participants.begin(), operation->participants.end(),
                                  [&](const auto& participant) {
                                    return participant.payload_kind !=
                                               OperationParticipantPayloadKind::kMotion ||
                                           !matches_scope(participant.binding.value());
                                  });
             })) {
    status.frozen = true;
    status.reason = error(std::string(to_string(ErrorCode::kInvalidStateTransition)),
                          "velocity group overlaps a different active execution scope",
                          "runtime_scale_group_scope_busy");
  } else if (group && active.size() == 1U &&
             active.front()->intent.coordination_policy ==
                 OperationCoordinationPolicy::kCoordinatedAll) {
    // Shared-start execution is removed: a synchronized motion never
    // has a shared clock, so its group velocity stays frozen.
    status.frozen = true;
    status.reason =
        error(std::string(to_string(ErrorCode::kInvalidStateTransition)),
              "synchronized velocity requires every participant ready on the shared clock",
              "coordinated_runtime_scale_not_ready");
  } else if (group && std::any_of(active.begin(), active.end(), [](const auto* operation) {
               return operation->state.state != OperationStateKind::kPlanning &&
                      operation->state.state != OperationStateKind::kValidating &&
                      operation->state.state != OperationStateKind::kPrepared &&
                      operation->state.state != OperationStateKind::kDispatching &&
                      operation->state.state != OperationStateKind::kExecuting;
             })) {
    status.frozen = true;
    status.reason = error(std::string(to_string(ErrorCode::kInvalidStateTransition)),
                          "a group motion is not in a velocity-scalable state",
                          "runtime_scale_group_state_invalid");
  }
  return Result<RuntimeScaleControlStatusDTO>::Ok(std::move(status));
}

Result<OperationAcceptedDTO> ResourceAddressedExecutionKernel::execute(
    const OperationIntentDTO& intent) {
  if (std::any_of(intent.participants.begin(), intent.participants.end(), [](const auto& row) {
        return std::holds_alternative<SetTcpParticipantIntentDTO>(row.payload);
      })) {
    return execute_set_tcp(intent);
  }
  // Lock scope (OD-67, point 5): submission_mutex_ orders admission only:
  // validation, the durable manual claim, resource reservation and
  // registration in active_operations_, durable acceptance plus the replay
  // entry. It linearizes that with runtime-scale settings (read at admission
  // with their value sequence), the replay ledgers, route/registration
  // reconfiguration, I/O-rule staging, blackboard writes and Reset/Apply
  // settled-work checks, which all take it too. end_admission() releases it
  // before a motion's subordinate Prepare (a later speed change reaches its
  // participant's ramp target, latest value wins, OD-72), before a motion
  // takeover's cancel and advance (try_accept_motion_takeover), and before a
  // command's advance/dispatch: from there on the operation is resident and
  // its own commit guard, flags and Stop fences order every effect, exactly
  // as on the scheduler's reconcile path.
  std::unique_lock submit_lock(submission_mutex_);
  bool manual_claim_acquired = false;
  bool pending_program_run_registered = false;
  bool admission_ended = false;
  const auto end_admission = [&] {
    if (admission_ended) return;
    admission_ended = true;
    if (pending_program_run_registered) {
      std::lock_guard state_lock(state_mutex_);
      if (pending_program_run_admission_ == std::optional<ProgramRunId>{intent.program_run_id}) {
        pending_program_run_admission_.reset();
      }
    }
    // Runs after pending work is cleared, but while Execute still owns
    // ordinary submission ordering.
    if (manual_claim_acquired) {
      try {
        static_cast<void>(reconcile_manual_operation_admission_locked());
      } catch (...) {
      }
      // An unsettled result is not retried here: the durable claim itself
      // keeps conflicting work closed. The existing reconciliation lane
      // retries release and surfaces an unresolved error; do not introduce a
      // second, permanently latched admission inhibit.
    }
    submit_lock.unlock();
  };
  const ScopeExit end_admission_on_return([&] { end_admission(); });

  auto validation = validate_operation_intent(intent);
  if (!validation.ok()) {
    return Result<OperationAcceptedDTO>::Err(validation.error());
  }
  if (intent.request_context.request_owner_epoch != config_.request_owner_epoch) {
    return Result<OperationAcceptedDTO>::Err(
        error("stale_request_owner_epoch", "operation request names a stale Kernel owner epoch",
              "stale_request_owner_epoch"));
  }
  const bool single_independent =
      intent.participants.size() == 1U &&
      intent.coordination_policy == OperationCoordinationPolicy::kIndependent;
  const bool coordinated =
      intent.participants.size() > 1U &&
      intent.coordination_policy == OperationCoordinationPolicy::kCoordinatedAll;
  if (!single_independent && !coordinated) {
    return reject<OperationAcceptedDTO>(
        "operation participant cardinality differs from its coordination policy",
        "operation_coordination_shape_invalid");
  }

  {
    std::lock_guard state_lock(state_mutex_);
    const auto replay =
        replay_ledger_.find(intent.request_context.request_id);
    if (replay != replay_ledger_.end()) {
      if (replay->intent != intent) {
        return Result<OperationAcceptedDTO>::Err(
            error("request_id_conflict", "operation request ID was reused with different intent",
                  "request_id_conflict"));
      }
      auto accepted = replay->accepted;
      accepted.replay_status = RequestReplayStatusDTO::kReplayedExact;
      return Result<OperationAcceptedDTO>::Ok(std::move(accepted));
    }
    // A retained receipt is observation of the original decision, not fresh
    // motion admission. Keep it readable while Stop/fault fences reject every
    // request absent from the replay ledger.
    if (motion_admission_closed_locked()) {
      return reject<OperationAcceptedDTO>(
          "new motion admission is closed until the execution host restarts",
          "motion_admission_closed", ErrorCode::kInvalidStateTransition);
    }
    // A cell latch refuses every node command, manual motion and manual I/O.
    if (auto refusal = cell_latch_refusal_locked()) {
      return Result<OperationAcceptedDTO>::Err(std::move(*refusal));
    }
    if (emergency_stop_reserve_.find(intent.request_context.request_id.value()).has_value()) {
      return Result<OperationAcceptedDTO>::Err(
          error("request_id_conflict", "operation request ID is retained by emergency Stop",
                "request_id_conflict"));
    }
    const auto control_request = control_replay_ledger_.find(intent.request_context.request_id);
    if (control_request != control_replay_ledger_.end()) {
      return Result<OperationAcceptedDTO>::Err(error(
          "request_id_conflict", "operation request ID was already used by an operation control",
          "request_id_conflict"));
    }
    if (stop_replay_ledger_.contains(intent.request_context.request_id) ||
        runtime_scale_control_replay_ledger_.contains(intent.request_context.request_id)) {
      return Result<OperationAcceptedDTO>::Err(error(
          "request_id_conflict", "operation request ID was already used by another Kernel command",
          "request_id_conflict"));
    }
    if (!make_operation_admission_capacity_locked()) {
      return Result<OperationAcceptedDTO>::Err(
          error("operation_capacity_full", "active operation capacity reached its fixed bound",
                "operation_capacity_full"));
    }
    if (const auto scope = validate_program_execution_scope_locked(intent); !scope.ok()) {
      return Result<OperationAcceptedDTO>::Err(scope.error());
    }
    if (pending_program_run_admission_.has_value()) {
      return reject<OperationAcceptedDTO>("another Program-run admission is already being prepared",
                                          "program_run_admission_invariant",
                                          ErrorCode::kInvariantViolation);
    }
    pending_program_run_admission_ = intent.program_run_id;
    pending_program_run_registered = true;
  }

  if (coordinated) {
    return execute_coordinated_motion_after_preflight(intent, end_admission);
  }

  auto resolved = resolve_registration(intent, 0U);
  {
    std::lock_guard state_lock(state_mutex_);
    if (motion_admission_closed_locked())
      return reject<OperationAcceptedDTO>(
          "new motion admission is closed until the execution host restarts",
          "motion_admission_closed", ErrorCode::kInvalidStateTransition);
  }
  if (!resolved.ok()) {
    return Result<OperationAcceptedDTO>::Err(resolved.error());
  }
  auto* engine = resolved.value().engine;
  if (engine == nullptr) {
    return reject<OperationAcceptedDTO>("resolved participant has no registered subordinate engine",
                                        "runtime_participant_engine_missing",
                                        ErrorCode::kInvariantViolation);
  }
  bool engine_already_in_use{};
  {
    std::lock_guard state_lock(state_mutex_);
    engine_already_in_use = std::any_of(
        active_operations_.begin(), active_operations_.end(),
        [&](const auto& item) { return item.second->participant_for(engine) != nullptr; });
    if (engine_already_in_use) {
      const auto* incoming =
          std::get_if<MotionParticipantIntentDTO>(&intent.participants.front().payload);
      const bool protected_owner =
          std::any_of(active_operations_.begin(), active_operations_.end(), [&](const auto& item) {
            const auto* row = item.second->participant_for(engine);
            return row && !row->io_rule_registrations.empty();
          });
      if (incoming && intent.participants.front().execution_scope &&
          !resolved.value().intrinsic_direct_control &&
          (protected_owner || !incoming->motion_interrupt_scopes.empty() ||
           resolved.value().captured_node_interrupt_scope.has_value()))
        return reject<OperationAcceptedDTO>(
            "moving takeover requires a qualified scoped-rule physical-selection join",
            "scoped_motion_takeover_unsupported", ErrorCode::kUnsupportedCapability);
    }
    if (engine_already_in_use && (resolved.value().intrinsic_direct_control ||
                                  !intent.participants.front().execution_scope ||
                                  !std::holds_alternative<MotionParticipantIntentDTO>(
                                      intent.participants.front().payload))) {
      return reject<OperationAcceptedDTO>(
          "the resolved single-participant engine already owns an unsettled operation",
          "participant_engine_busy");
    }
  }
  if (next_operation_id_ == 0U || next_operation_id_ == std::numeric_limits<std::uint64_t>::max()) {
    return reject<OperationAcceptedDTO>("operation identity space is exhausted",
                                        "operation_id_exhausted", ErrorCode::kInternalError);
  }

  const auto& binding = resolved.value().binding.value();
  if (!binding.control_path.has_value()) {
    return reject<OperationAcceptedDTO>("resolved participant has no active control path",
                                        "runtime_participant_control_path_missing",
                                        ErrorCode::kMissingReference);
  }
  const auto* control = &*binding.control_path;
  if (runtime_route_configuration_publication_required_.contains(control->control_domain_id)) {
    return reject<OperationAcceptedDTO>(
        "runtime route configuration has not been published after reconfiguration",
        "runtime_route_publication_required", ErrorCode::kInvalidStateTransition);
  }
  const auto route_readback =
      runtime_route_configuration_readbacks_.find(control->control_domain_id);
  if (route_readback != runtime_route_configuration_readbacks_.end() &&
      (!route_readback->second.operation_admission_open ||
       route_readback->second.active_configuration.active_control_configuration_id !=
           control->active_control_configuration_id ||
       route_readback->second.active_configuration.active_control_configuration_generation !=
           control->active_control_configuration_generation)) {
    return reject<OperationAcceptedDTO>("runtime route configuration is unreconciled or stale",
                                        "runtime_route_configuration_not_ready",
                                        ErrorCode::kInvalidStateTransition);
  }
  if (auto refusal = component_command_refusal_locked(resolved.value()); refusal.has_value()) {
    return Result<OperationAcceptedDTO>::Err(std::move(*refusal));
  }
  if (!resolved.value().stop_readiness.has_value()) {
    return reject<OperationAcceptedDTO>("resolved participant has no verified Stop readiness",
                                        "runtime_participant_stop_readiness_missing",
                                        ErrorCode::kMissingReference);
  }
  std::vector<ResourceId> resources;
  resources.reserve(binding.resources.size());
  for (const auto& resource : binding.resources) {
    resources.push_back(resource.resource_id);
  }
  const auto exact_identity = runtime_participant_binding_identity(binding);
  if (!exact_identity.ok()) return Result<OperationAcceptedDTO>::Err(exact_identity.error());
  const auto operation_id = OperationId("operation." + config_.request_owner_epoch.value() + "." +
                                        std::to_string(next_operation_id_++));
  std::shared_ptr<const ResolvedManualProgramHandover> manual_handover;
  if (intent.manual_handover) {
    if (!resolved.value().intrinsic_direct_control) {
      return reject<OperationAcceptedDTO>("only intrinsic manual work can name a held Program",
                                          "direct_control_product_mismatch");
    }
    auto held = resolve_manual_program_handover(intent);
    if (!held.ok()) return Result<OperationAcceptedDTO>::Err(held.error());
    if (!held.value()) {
      return reject<OperationAcceptedDTO>("manual handover has no current Program",
                                          "manual_handover_scope_stale");
    }
    const auto& capture = held.value()->capture;
    const ManualProgramHandoverContextDTO current_context{
        capture.program_runner_owner_epoch, capture.program_run_id, capture.claim_generation,
        held.value()->selection_revision};
    if (current_context != *intent.manual_handover) {
      return reject<OperationAcceptedDTO>("manual handover differs from its current hold",
                                          "manual_handover_scope_stale");
    }
    manual_handover =
        std::make_shared<const ResolvedManualProgramHandover>(std::move(*held.value()));
  }
  if (resolved.value().intrinsic_direct_control) {
    if (intent.participants.front().execution_scope.has_value()) {
      return reject<OperationAcceptedDTO>("manual work cannot carry Program-node authority",
                                          "direct_control_product_mismatch");
    }
    auto claim_resources = resources;
    std::sort(claim_resources.begin(), claim_resources.end());
    using content_identity::CanonicalValue;
    const auto digest = content_identity::sha256_digest(
        content_identity::canonical_json(CanonicalValue(CanonicalValue::Object{
            {"schema", CanonicalValue("manyforge.manual_operation_admission.v1")},
            {"operation_id", CanonicalValue(operation_id.value())},
            {"program_binding_id", CanonicalValue(intent.program_binding_id.value())},
            {"program_run_id", CanonicalValue(intent.program_run_id.value())},
        })));
    std::lock_guard admission_lock(*admission_mutex_);
    const auto current = cell_admission_owner_->GetAuthoringAdmission();
    CellAdmissionClaimIntentDTO claim_intent{
        CellAdmissionClaimKindDTO::kManualOperation,
        CellAdmissionOwnerRoleDTO::kManualOperationOwner,
        config_.request_owner_epoch,
        operation_id.value(),
        intent.request_context.request_id,
        ContentDigest(digest),
        current.admission_generation,
        {std::move(claim_resources),
         std::vector<CellAdmissionConfigurationScopeDTO>{
             {control->control_domain_id, control->active_control_configuration_id,
              control->active_control_configuration_generation}}},
        std::nullopt,
        std::nullopt};
    if (manual_handover) {
      const auto& context = *intent.manual_handover;
      const auto offer_digest = content_identity::sha256_digest(
          content_identity::canonical_json(CanonicalValue(CanonicalValue::Object{
              {"schema", CanonicalValue("manyforge.manual_program_handover.v1")},
              {"operation_digest", CanonicalValue(digest)},
              {"program_runner_owner_epoch",
               CanonicalValue(context.program_runner_owner_epoch.value())},
              {"program_run_id", CanonicalValue(context.program_run_id.value())},
              {"claim_generation",
               CanonicalValue(static_cast<double>(context.claim_generation.value()))},
              {"selection_revision",
               CanonicalValue(static_cast<double>(context.selection_revision))},
          })));
      claim_intent.intervention_evidence = CellAdmissionInterventionEvidenceDTO{
          CellAdmissionOwnerRoleDTO::kManualOperationOwner, config_.request_owner_epoch,
          operation_id.value(), ContentDigest(offer_digest), context.claim_generation};
    }
    const auto claim =
        manual_handover
            ? cell_admission_owner_->AcquireManualCellAdmissionClaim(
                  claim_intent,
                  [&](const auto&, const auto& parent) -> Result<void> {
                    if (!parent ||
                        parent->claim_generation != manual_handover->capture.claim_generation)
                      return reject<void>("manual parent changed", "manual_handover_scope_stale");
                    std::lock_guard state_lock(state_mutex_);
                    return validate_manual_program_handover_locked(*manual_handover);
                  })
            : cell_admission_owner_->AcquireCellAdmissionClaim(claim_intent);
    if (!claim.ok()) return Result<OperationAcceptedDTO>::Err(claim.error());
    refresh_execution_claims();
    manual_claim_acquired = true;
  }
  const auto participant_id = OperationParticipantId(operation_id.value() + ".participant.1");
  const auto controller_goal_id = ControllerGoalId(operation_id.value() + ".controller-goal.1");
  const ControllerGoalGeneration controller_goal_generation(1U);
  const OperationRevision operation_revision(1U);
  const auto payload_kind =
      std::holds_alternative<MotionParticipantIntentDTO>(intent.participants.front().payload)
          ? OperationParticipantPayloadKind::kMotion
          : OperationParticipantPayloadKind::kCapabilityCommand;
  if (payload_kind == OperationParticipantPayloadKind::kMotion &&
      !resolved.value().participant_policy.has_value()) {
    return reject<OperationAcceptedDTO>("resolved participant has no sealed motion policy",
                                        "runtime_participant_policy_missing",
                                        ErrorCode::kMissingReference);
  }
  if (resolved.value().prepared_motion_engine == nullptr) {
    return reject<OperationAcceptedDTO>("resolved participant has no prepared execution engine",
                                        "runtime_participant_prepared_engine_missing",
                                        ErrorCode::kMissingReference);
  }
  ResourceSlotOwnerDTO slot_owner{operation_id,
                                  operation_revision,
                                  participant_id,
                                  control->control_domain_id,
                                  control->control_domain_revision,
                                  control->control_binding_id,
                                  control->control_binding_revision,
                                  control->controller_topology_id,
                                  control->topology_revision,
                                  control->control_route_id,
                                  control->active_control_configuration_id,
                                  control->active_control_configuration_generation,
                                  control->containment_granularity};
  std::optional<ExecuteRequestDTO> motion_request;
  std::optional<double> requested_tcp_linear_speed_mps;
  std::optional<GripperCommandRequestDTO> command_request;
  if (payload_kind == OperationParticipantPayloadKind::kMotion) {
    const auto& motion = std::get<MotionParticipantIntentDTO>(intent.participants.front().payload);
    requested_tcp_linear_speed_mps = motion.speed_profile.max_tcp_linear_speed_mps;
    if (control->runtime_scale_qualification.has_value() &&
        runtime_scale_availability_of(engine) ==
            RuntimeScaleMechanismAvailability::kControllerNotListening) {
      return reject<OperationAcceptedDTO>(
          "the motion's controller is not listening for runtime scale input",
          kRuntimeScaleControllerNotListening, ErrorCode::kUnsupportedCapability);
    }
    double initial_runtime_scale = 1.0;
    std::uint64_t initial_runtime_scale_sequence = 0U;
    {
      // The seed and its order are taken together: a speed change after
      // this read has a later sequence and wins at the participant (OD-72).
      std::lock_guard state_lock(state_mutex_);
      initial_runtime_scale = runtime_scale_setting_for_binding_locked(binding);
      initial_runtime_scale_sequence = next_runtime_scale_value_sequence();
    }
    auto built =
        build_motion_request(intent, intent.participants.front(), motion, binding, operation_id,
                             operation_revision, participant_id, controller_goal_id,
                             controller_goal_generation, resolved.value(), initial_runtime_scale,
                             initial_runtime_scale_sequence);
    if (!built.ok()) {
      return Result<OperationAcceptedDTO>::Err(built.error());
    }
    motion_request = std::move(built.value());
    motion_request->trajectory_relative_timeout = resolved.value().intrinsic_direct_control;
  } else {
    const auto& command = std::get<CapabilityCommandIntentDTO>(intent.participants.front().payload);
    auto built = build_command_request(
        command, binding, engine->gripper_actuation_kind(binding.command_target_id.value()),
        engine->gripper_output_mode(binding.command_target_id.value()));
    if (!built.ok()) {
      return Result<OperationAcceptedDTO>::Err(built.error());
    }
    command_request = std::move(built.value());
    auto private_validation = engine->validate_gripper_command(*command_request);
    if (!private_validation.ok()) {
      return Result<OperationAcceptedDTO>::Err(private_validation.error());
    }
  }
  const auto accepted_at_ns = clock_.now();
  const OperationAcceptedDTO accepted{intent.request_context.request_id,
                                      RequestReplayStatusDTO::kAcceptedNew,
                                      operation_id,
                                      operation_revision,
                                      {OperationParticipantAssignmentDTO{0U, participant_id}},
                                      accepted_at_ns};
  std::optional<RuntimeScaleStatusDTO> initial_runtime_scale_status;
  if (payload_kind == OperationParticipantPayloadKind::kMotion &&
      motion_request->initial_runtime_scale.has_value()) {
    const auto initial_runtime_scale = motion_request->initial_runtime_scale->requested_scale;
    initial_runtime_scale_status =
        RuntimeScaleStatusDTO{operation_id,
                              operation_revision,
                              RuntimeScaleGeneration(0U),
                              initial_runtime_scale,
                              initial_runtime_scale,
                              {initial_scale_status(*motion_request->initial_runtime_scale,
                                                    control->provider_runtime_epoch)},
                              RuntimeScaleApplicationState::kPending,
                              accepted_at_ns};
  }
  OperationParticipantStateDTO participant_state{participant_id,
                                                 controller_goal_id,
                                                 controller_goal_generation,
                                                 binding.controllable_component_id,
                                                 binding.command_target_id,
                                                 binding.capability_id,
                                                 exact_identity.value(),
                                                 payload_kind,
                                                 OperationStateKind::kResolving,
                                                 0.0,
                                                 resources,
                                                 std::nullopt,
                                                 intent.participants.front().execution_scope};
  OperationStateDTO operation_state_dto{operation_id,
                                        operation_revision,
                                        intent.program_binding_id,
                                        intent.program_run_id,
                                        intent.coordination_policy,
                                        OperationStateKind::kResolving,
                                        {std::move(participant_state)},
                                        accepted_at_ns,
                                        std::nullopt,
                                        std::nullopt,
                                        std::nullopt};
  if (initial_runtime_scale_status.has_value()) {
    operation_state_dto.runtime_scale_generation = RuntimeScaleGeneration(0U);
  }

  std::optional<PreparedParticipantStopPlanDTO> ordinary_stop_plan;
  std::optional<PreparedParticipantStopPlanDTO> fault_stop_plan;
  {
    PreparedParticipantStopPlanContextDTO stop_context{
        operation_id,
        operation_revision,
        participant_id,
        intent.program_id,
        intent.program_revision,
        intent.program_binding_id,
        intent.program_run_id,
        payload_kind == OperationParticipantPayloadKind::kMotion
            ? resolved.value().default_operator_stop_profile
            : std::optional<MotionInterruptStopProfileDTO>{},
        payload_kind == OperationParticipantPayloadKind::kMotion &&
                resolved.value().default_operator_stop_profile.has_value()
            ? ParticipantStopProfilePurposeDTO::kOperator
            : ParticipantStopProfilePurposeDTO::kOrdinary,
        std::nullopt,
        resolved.value().stop_node_category};
    auto made_ordinary_stop_plan = MakePreparedParticipantStopPlan(
        resolved.value().stop_readiness->value().resolved_stop_base_plan, std::move(stop_context),
        binding.resources, resolved.value().retained_default_stop_limits);
    if (!made_ordinary_stop_plan.ok()) {
      return Result<OperationAcceptedDTO>::Err(made_ordinary_stop_plan.error());
    }
    if (payload_kind == OperationParticipantPayloadKind::kMotion &&
        !resolved.value().default_fault_stop_profile.has_value() &&
        controller_goal_cancel_stage_index(made_ordinary_stop_plan.value()) ==
            made_ordinary_stop_plan.value().stage_commands.size()) {
      return reject<OperationAcceptedDTO>(
          "motion without a fault Stop profile requires a qualified controller-goal cancel",
          "runtime_collision_cancel_unqualified", ErrorCode::kUnsupportedCapability);
    }
    auto made_fault_stop_plan = make_runtime_fault_stop_plan(
        *resolved.value().stop_readiness,
        payload_kind == OperationParticipantPayloadKind::kMotion
            ? resolved.value().default_fault_stop_profile
            : std::optional<MotionInterruptStopProfileDTO>{},
        binding, resolved.value().retained_default_stop_limits, resolved.value().stop_node_category,
        operation_id, operation_revision, participant_id, intent.program_id,
        intent.program_revision, intent.program_binding_id, intent.program_run_id);
    if (!made_fault_stop_plan.ok()) {
      return Result<OperationAcceptedDTO>::Err(made_fault_stop_plan.error());
    }

    ordinary_stop_plan = std::move(made_ordinary_stop_plan.value());
    fault_stop_plan = std::move(made_fault_stop_plan.value());
  }

  std::optional<PreparedOperationParticipantDTO> prepared_command_product;
  if (payload_kind == OperationParticipantPayloadKind::kCapabilityCommand) {
    auto made = build_prepared_capability_command_participant(
        intent, std::get<CapabilityCommandIntentDTO>(intent.participants.front().payload),
        resolved.value().binding, *resolved.value().stop_readiness,
        resolved.value().retained_default_stop_limits, resolved.value().stop_node_category,
        operation_id, operation_revision, participant_id, *ordinary_stop_plan);
    if (!made.ok()) {
      return Result<OperationAcceptedDTO>::Err(made.error());
    }
    prepared_command_product = std::move(made.value());
  }

  std::shared_ptr<const PreparedParticipantStopPlanDTO> retained_stop_plan;
  std::shared_ptr<const PreparedParticipantStopPlanDTO> retained_fault_stop_plan;
  std::shared_ptr<const PreparedOperationParticipantDTO> retained_command_product;
  std::map<OperationId, std::unique_ptr<ActiveOperation>>::node_type prepared_operation;
  std::shared_ptr<const detail::MotionResumeIntent> continuation;
  ReplayEntry prepared_replay{intent, accepted};
  std::vector<OperationId> protected_operation_ids;
  try {
    if (ordinary_stop_plan)
      retained_stop_plan =
          std::make_shared<const PreparedParticipantStopPlanDTO>(std::move(*ordinary_stop_plan));
    if (fault_stop_plan)
      retained_fault_stop_plan =
          std::make_shared<const PreparedParticipantStopPlanDTO>(std::move(*fault_stop_plan));
    if (prepared_command_product.has_value()) {
      retained_command_product = std::make_shared<const PreparedOperationParticipantDTO>(
          std::move(*prepared_command_product));
    }
    std::map<OperationId, std::unique_ptr<ActiveOperation>> staging;
    staging.emplace(operation_id,
                    std::make_unique<ActiveOperation>(ActiveOperation{
                        intent,
                        accepted,
                        {ActiveOperationParticipant{resolved.value().binding,
                                                    resolved.value().stop_readiness,
                                                    resolved.value().retained_default_stop_limits,
                                                    resolved.value().stop_node_category,
                                                    resolved.value().default_operator_stop_profile,
                                                    retained_stop_plan,
                                                    resolved.value().default_fault_stop_profile,
                                                    retained_fault_stop_plan,
                                                    std::nullopt,
                                                    engine,
                                                    resolved.value().prepared_motion_engine,
                                                    resolved.value().stop_execution_port,
                                                    slot_owner,
                                                    resources,
                                                    payload_kind,
                                                    ResourceSlotState::kReserved,
                                                    0U,
                                                    0U,
                                                    0U,
                                                    false,
                                                    false,
                                                    false,
                                                    false,
                                                    false,
                                                    false,
                                                    std::nullopt,
                                                    motion_request}},
                        operation_state_dto,
                        std::nullopt,
                        false,
                        true,
                        false,
                        false,
                        std::move(initial_runtime_scale_status)}));
    staging.at(operation_id)->participants.front().engine_subscription_id =
        resolved.value().engine_subscription_id;
    staging.at(operation_id)->participants.front().io_motion_rule_policies =
        resolved.value().io_motion_rule_policies;
    if (payload_kind == OperationParticipantPayloadKind::kCapabilityCommand)
      staging.at(operation_id)->participants.front().gripper_actuation =
          engine->gripper_actuation_kind(binding.command_target_id.value());
    stamp_operation_capture_locked(*staging.at(operation_id), operation_state_dto);
    staging.at(operation_id)->manual_handover = manual_handover;
    staging.at(operation_id)->manual = resolved.value().intrinsic_direct_control;
    staging.at(operation_id)->state = operation_state_dto;
    if (payload_kind == OperationParticipantPayloadKind::kMotion && engine_already_in_use) {
      auto& participant = staging.at(operation_id)->participants.front();
      participant.takeover_preparation.emplace(ResolvedPreparedMotionExecutionRequestDTO{
          *motion_request, operation_revision, resolved.value().binding,
          *resolved.value().stop_readiness, requested_tcp_linear_speed_mps,
          *resolved.value().deployment_maximum_tcp_linear_speed_mps,
          resolved.value().retained_default_stop_limits, resolved.value().stop_node_category,
          retained_stop_plan, retained_fault_stop_plan, continuation});
      participant.takeover_preparation_budget_ns =
          motion_takeover_budget_ns(resolved.value().motion_takeover_preparation_budget_s);
      participant.takeover_planning_budget_ns =
          motion_takeover_planning_budget_ns(resolved.value().motion_takeover_planning_budget_s,
                                             resolved.value().motion_takeover_preparation_budget_s);
    }
    prepared_operation = staging.extract(operation_id);
  } catch (...) {
    return reject<OperationAcceptedDTO>(
        "the bounded operation admission state could not be allocated before reservation",
        "operation_admission_capacity_unavailable", ErrorCode::kInternalError);
  }

  const auto takeover = try_accept_motion_takeover(prepared_operation, prepared_replay,
                                                   resolved.value(), end_admission);
  if (!takeover.ok()) return Result<OperationAcceptedDTO>::Err(takeover.error());
  if (takeover.value()) return Result<OperationAcceptedDTO>::Ok(*takeover.value());

  {
    // This is the only Execute/Stop ordering section: publish the reserved
    // pending operation before any ordinary journal or provider call. A Stop
    // can now cancel/fence it without waiting for those calls.
    // Acquire durable admission ordering before Kernel state. A pending
    // authoring claim may delay new Execute, but it must not make Execute hold
    // state while waiting and thereby delay recognized Stop.
    std::lock_guard admission_lock(*admission_mutex_);
    std::lock_guard state_lock(state_mutex_);
    // Close may retire command eligibility after this Execute's preflight.
    // Repeat the existing component check at the actual admission barrier,
    // before any resource reservation, journal write or provider effect.
    if (auto refusal = component_command_refusal_locked(resolved.value())) {
      return Result<OperationAcceptedDTO>::Err(std::move(*refusal));
    }
    if (motion_admission_closed_locked()) {
      return reject<OperationAcceptedDTO>(
          "new motion admission closed while the operation was being prepared",
          "motion_admission_closed", ErrorCode::kInvalidStateTransition);
    }
    if (const auto scope = validate_program_execution_scope_locked(
            intent, prepared_operation.mapped()->participants.front().non_motion_transaction(),
            resolved.value().intrinsic_direct_control);
        !scope.ok()) {
      return Result<OperationAcceptedDTO>::Err(scope.error());
    }
    // Ordinary Run work only; Manual Robot Control is not attention-gated.
    if (!resolved.value().intrinsic_direct_control) {
      if (auto refusal = operator_alarm_attention_refusal_locked(resources)) {
        return Result<OperationAcceptedDTO>::Err(std::move(*refusal));
      }
    }
    if (manual_handover) {
      const auto current_hold =
          validate_manual_program_handover_locked(*manual_handover);
      if (!current_hold.ok()) return Result<OperationAcceptedDTO>::Err(current_hold.error());
    }
    if (emergency_stop_reserve_.find(intent.request_context.request_id.value()).has_value()) {
      return Result<OperationAcceptedDTO>::Err(
          error("request_id_conflict", "operation request ID became retained by emergency Stop",
                "request_id_conflict"));
    }
    const bool engine_in_use =
        immediate_step_engines_.contains(engine) ||
        std::any_of(active_operations_.begin(), active_operations_.end(),
                    [&](const auto& item) { return item.second->participant_for(engine) != nullptr; });
    const bool request_id_used =
        replay_ledger_.contains(intent.request_context.request_id) ||
        control_replay_ledger_.contains(intent.request_context.request_id) ||
        stop_replay_ledger_.contains(intent.request_context.request_id) ||
        runtime_scale_control_replay_ledger_.contains(intent.request_context.request_id);
    if (request_id_used) {
      return Result<OperationAcceptedDTO>::Err(
          error("request_id_conflict", "operation request ID was used during admission",
                "request_id_conflict"));
    }
    if (engine_in_use) {
      return reject<OperationAcceptedDTO>(
          "the resolved single-participant engine became busy during admission",
          "participant_engine_busy");
    }
    if (!make_operation_admission_capacity_locked()) {
      return Result<OperationAcceptedDTO>::Err(
          error("operation_capacity_full", "active operation capacity reached its fixed bound",
                "operation_capacity_full"));
    }
    if (program_cycle_claim_run_ && program_cycle_claim_run_->capture &&
        program_cycle_claim_run_->program_run_id == intent.program_run_id &&
        intent.participants.front().execution_scope) {
      const auto& nodes = program_cycle_claim_run_->capture->table->nodes;
      const auto& scope = *intent.participants.front().execution_scope;
      const auto node =
          std::lower_bound(nodes.begin(), nodes.end(), scope.node_instance_id,
                           [](const auto& row, const auto& id) { return row.instance_id < id; });
      const auto index = static_cast<std::size_t>(node - nodes.begin());
      if (index < program_cycle_claim_run_->motion_continuations.size()) {
        const auto& pending = program_cycle_claim_run_->motion_continuations[index];
        if (pending) {
          if (node->owner_api != "kernel.operation" ||
              pending->origin.node_instance_id != scope.node_instance_id ||
              pending->origin.node_attempt_id == scope.node_attempt_id ||
              payload_kind != OperationParticipantPayloadKind::kMotion ||
              pending->binding.value() != resolved.value().binding.value() || !motion_request ||
              pending->base_frame != motion_request->binding.base_frame ||
              pending->tcp_frame != motion_request->binding.tcp_frame ||
              pending->planner_capability != motion_request->binding.planner_capability_binding ||
              pending->authored_motion_kind != motion_request->motion.index())
            return reject<OperationAcceptedDTO>(
                "new attempt differs from its retained motion continuation",
                "program_motion_continuation_mismatch", ErrorCode::kInvalidRequest);
          continuation = pending->intent;
          auto& participant = prepared_operation.mapped()->participants.front();
          participant.continuation = continuation;
          participant.continuation_node_index = index;
        }
      }
    }
    auto reservation =
        config_.operation_admission_gate != nullptr
            ? config_.operation_admission_gate->reserve(*resource_slots_, slot_owner, resources)
            : resource_slots_->reserve(slot_owner, resources);
    if (!reservation.ok()) {
      return Result<OperationAcceptedDTO>::Err(reservation.error());
    }
    const auto inserted = active_operations_.insert(std::move(prepared_operation));
    if (!inserted.inserted) {
      const auto released = resource_slots_->release(slot_owner, resources);
      if (!released.ok()) {
        return Result<OperationAcceptedDTO>::Err(released.error());
      }
      return reject<OperationAcceptedDTO>("operation identity is already resident",
                                          "operation_id_conflict", ErrorCode::kInvariantViolation);
    }
    protected_operation_ids = durable_operation_retention_ids_locked();
  }

  OperationCommitGuard admission_commit(*this, operation_id);
  auto durable_acceptance = attempt_journal_write([&] {
    return journal_.RecordOperationAccepted(config_.request_owner_epoch, intent, accepted,
                                            operation_state_dto, protected_operation_ids);
  });
  if (!durable_acceptance.ok()) {
    std::lock_guard state_lock(state_mutex_);
    retain_operation_journal_write_fault_locked("operation_accepted", durable_acceptance.error(),
                                                &operation_state_dto,
                                                intent.request_context.request_id);
    auto* active_operation = find_active_operation_locked(operation_id);
    if (active_operation != nullptr) {
      active_operation->ordinary_acceptance_pending = false;
      if (!active_operation->state.active_stop_id.has_value()) {
        auto& participant = active_operation->participants.front();
        const auto released =
            resource_slots_->release(participant.slot_owner, participant.resources);
        if (!released.ok()) {
          return Result<OperationAcceptedDTO>::Err(released.error());
        }
        active_operations_.erase(operation_id);
      }
    }
    return Result<OperationAcceptedDTO>::Err(durable_acceptance.error());
  }

  bool dispatch_allowed = false;
  {
    std::lock_guard state_lock(state_mutex_);
    auto* active_operation = find_active_operation_locked(operation_id);
    if (active_operation == nullptr) {
      return reject<OperationAcceptedDTO>("operation disappeared after durable acceptance",
                                          "active_operation_lost", ErrorCode::kInvariantViolation);
    }
    active_operation->ordinary_acceptance_pending = false;
    active_operation->ordinary_acceptance_durable = true;
    replay_ledger_.push_back(std::move(prepared_replay));
    auto& participant = active_operation->participants.front();
    dispatch_allowed = !participant.stop_owns_reconciliation() &&
                       participant.slot_state == ResourceSlotState::kReserved;
    if (dispatch_allowed) {
      participant.prepare_in_flight = true;
    }
  }
  admission_commit.release();
  if (!dispatch_allowed) {
    return Result<OperationAcceptedDTO>::Ok(accepted);
  }

  Result<void> dispatch_result = Result<void>::Ok();
  if (payload_kind == OperationParticipantPayloadKind::kMotion) {
    // The motion is resident (registered and reserved above, so Apply and
    // Reset settled-work checks see it): release submission ordering before
    // its subordinate Prepare and dispatch (point 5). A speed change from
    // here on reaches its participant's ramp target, which the motion reads
    // when it registers its ramp (OD-72).
    end_admission();
    ResolvedPreparedMotionExecutionRequestDTO prepare_request{
        *motion_request,
        operation_revision,
        resolved.value().binding,
        *resolved.value().stop_readiness,
        requested_tcp_linear_speed_mps,
        *resolved.value().deployment_maximum_tcp_linear_speed_mps,
        resolved.value().retained_default_stop_limits,
        resolved.value().stop_node_category,
        retained_stop_plan,
        retained_fault_stop_plan,
        continuation};
    auto prepared_execution = [&]() -> Result<std::uint64_t> {
      for (const auto& scope : motion_request->motion_interrupt_scopes) {
        const auto policy = std::find_if(
            resolved.value().io_motion_rule_policies.begin(),
            resolved.value().io_motion_rule_policies.end(),
            [&](const auto& value) { return value.policy_id.value() == scope.policy_id; });
        if (policy == resolved.value().io_motion_rule_policies.end() || !policy->semantics)
          return reject<std::uint64_t>("prepared motion lost its admitted interrupt action",
                                       "motion_interrupt_action_not_admitted",
                                       ErrorCode::kInvalidRequest);
        prepare_request.interrupt_scope_actions.push_back(
            {MotionInterruptScopeId(scope.scope_id), policy->semantics->preemption_action,
             policy->stop_entry_stage_id, policy->success_through_stage_id});
      }
      return resolved.value().prepared_motion_engine->PrepareMotion(std::move(prepare_request));
    }();
    if (!prepared_execution.ok()) {
      dispatch_result = Result<void>::Err(prepared_execution.error());
    } else {
      bool cancel_preparation = false;
      {
        OperationCommitGuard commit(*this, operation_id);
        std::unique_lock state_lock(state_mutex_);
        auto* active_operation = find_active_operation_locked(operation_id);
        if (active_operation == nullptr) {
          cancel_preparation = true;
          dispatch_result =
              Result<void>::Err(error(std::string(to_string(ErrorCode::kInvariantViolation)),
                                      "active operation disappeared during subordinate admission",
                                      "active_operation_lost"));
        } else {
          cancel_preparation = active_operation->terminal_stop_result().has_value();
          // A timed-out Stop keeps this call's borrow until its late product
          // has been disposed of. Its immutable certificate cannot do cleanup.
          active_operation->participants.front().prepare_in_flight = cancel_preparation;
          if (!cancel_preparation) wake_stop_after_preparation_borrow_locked(operation_id);
        }
        auto* participant =
            active_operation == nullptr ? nullptr : &active_operation->participants.front();
        if (participant != nullptr && participant->subordinate_id != 0U &&
            participant->subordinate_id != prepared_execution.value()) {
          dispatch_result =
              Result<void>::Err(error(std::string(to_string(ErrorCode::kInvariantViolation)),
                                      "subordinate event and execute result identities disagree",
                                      "subordinate_execution_id_mismatch"));
        } else if (participant != nullptr) {
          participant->subordinate_id = prepared_execution.value();
          participant->preparation_id = prepared_execution.value();
          if (active_operation->state.state == OperationStateKind::kResolving) {
            auto planning_state = active_operation->state;
            planning_state.state = OperationStateKind::kPlanning;
            planning_state.participants.front().state = OperationStateKind::kPlanning;
            planning_state.updated_at_ns = clock_.now();
            auto durable_state =
                record_operation_state_locked(state_lock, planning_state,
                                              active_operation->runtime_scale_status.has_value()
                                                  ? &*active_operation->runtime_scale_status
                                                  : nullptr,
                                              "subordinate_motion_admitted");
            if (durable_state.ok() && durable_state.value()) {
              active_operation->state = std::move(planning_state);
            } else if (!durable_state.ok()) {
              dispatch_result = Result<void>::Err(durable_state.error());
            }
          }
        }
      }
      if (cancel_preparation) {
        const auto cancelled = resolved.value().prepared_motion_engine->CancelPreparedMotion(
            prepared_execution.value());
        if (!cancelled.ok()) {
          dispatch_result = Result<void>::Err(cancelled.error());
        }
      }
    }
    if (dispatch_result.ok()) {
      dispatch_result = advance_prepared_motion(operation_id, engine);
    }
  } else {
    ResolvedPreparedCapabilityCommandExecutionRequestDTO prepare_request{
        retained_command_product,
        resolved.value().binding,
        *resolved.value().stop_readiness,
        resolved.value().retained_default_stop_limits,
        resolved.value().stop_node_category,
        *command_request};
    auto prepared_execution = resolved.value().prepared_motion_engine->PrepareCapabilityCommand(
        std::move(prepare_request));
    if (!prepared_execution.ok()) {
      dispatch_result = Result<void>::Err(prepared_execution.error());
    } else {
      bool cancel_preparation = false;
      {
        std::lock_guard state_lock(state_mutex_);
        auto* active_operation = find_active_operation_locked(operation_id);
        if (active_operation == nullptr) {
          cancel_preparation = true;
          dispatch_result = Result<void>::Err(error(
              std::string(to_string(ErrorCode::kInvariantViolation)),
              "active command disappeared during subordinate admission", "active_operation_lost"));
        } else {
          auto& participant = active_operation->participants.front();
          cancel_preparation = participant.stop_owns_reconciliation() &&
                               active_operation->terminal_stop_result().has_value();
          participant.prepare_in_flight = cancel_preparation;
          if (!cancel_preparation) wake_stop_after_preparation_borrow_locked(operation_id);
          if (participant.subordinate_id != 0U &&
              participant.subordinate_id != prepared_execution.value()) {
            dispatch_result =
                Result<void>::Err(error(std::string(to_string(ErrorCode::kInvariantViolation)),
                                        "prepared command identity disagrees with resident state",
                                        "prepared_command_identity_mismatch"));
          } else {
            participant.subordinate_id = prepared_execution.value();
            participant.preparation_id = prepared_execution.value();
          }
        }
      }
      if (cancel_preparation) {
        const auto cancelled =
            resolved.value().prepared_motion_engine->CancelPreparedCapabilityCommand(
                prepared_execution.value());
        if (!cancelled.ok()) {
          dispatch_result = Result<void>::Err(cancelled.error());
        }
      }
    }
    if (dispatch_result.ok()) {
      // Release submission ordering before the provider command (the gripper
      // publish or goal send) (OD-67).
      end_admission();
      dispatch_result = advance_prepared_capability_command(operation_id, engine);
    }
  }

  {
    std::lock_guard state_lock(state_mutex_);
    if (auto* operation = find_active_operation_locked(operation_id)) {
      operation->participants.front().prepare_in_flight = false;
      settle_completed_control_locked(operation_id);
      wake_stop_after_preparation_borrow_locked(operation_id);
    }
  }
  if (!dispatch_result.ok()) {
    OperationCommitGuard commit(*this, operation_id);
    std::unique_lock state_lock(state_mutex_);
    auto* active_operation = find_active_operation_locked(operation_id);
    if (active_operation != nullptr) {
      auto& participant = active_operation->participants.front();
      participant.dispatch_effect_in_flight = false;
      if (participant.stop_owns_reconciliation()) {
        participant.last_subordinate_error = dispatch_result.error();
        return Result<OperationAcceptedDTO>::Ok(accepted);
      }
      auto failed_state = active_operation->state;
      failed_state.state = OperationStateKind::kInterventionRequired;
      failed_state.participants.front().state = OperationStateKind::kInterventionRequired;
      failed_state.participants.front().terminal_error = dispatch_result.error();
      failed_state.updated_at_ns = clock_.now();
      auto durable_failure =
          record_operation_state_locked(state_lock, failed_state,
                                        active_operation->runtime_scale_status.has_value()
                                            ? &*active_operation->runtime_scale_status
                                            : nullptr,
                                        "subordinate_dispatch_failed_after_acceptance");
      if (durable_failure.ok() && durable_failure.value()) {
        if (participant.slot_state != ResourceSlotState::kTerminal) {
          const auto transition = resource_slots_->transition(
              participant.slot_owner, participant.resources, ResourceSlotState::kTerminal);
          if (transition.ok()) {
            participant.slot_state = ResourceSlotState::kTerminal;
          }
        }
        active_operation->state = std::move(failed_state);
      }
    }
    return Result<OperationAcceptedDTO>::Ok(accepted);
  }
  return Result<OperationAcceptedDTO>::Ok(accepted);
}

Result<OperationAcceptedDTO>
ResourceAddressedExecutionKernel::execute_coordinated_motion_after_preflight(
    const OperationIntentDTO& intent, const std::function<void()>& end_admission) {
  struct PreparedAdmission final {
    RuntimeParticipantExecutionRegistration registration;
    OperationParticipantPayloadKind payload_kind{OperationParticipantPayloadKind::kMotion};
    std::optional<ExecuteRequestDTO> motion_request;
    std::optional<double> requested_tcp_linear_speed_mps;
    std::optional<GripperCommandRequestDTO> command_request;
    std::shared_ptr<const PreparedOperationParticipantDTO> prepared_command_product;
    std::shared_ptr<const PreparedParticipantStopPlanDTO> stop_plan;
    std::shared_ptr<const PreparedParticipantStopPlanDTO> fault_stop_plan;
  };

  if (next_operation_id_ == 0U || next_operation_id_ == std::numeric_limits<std::uint64_t>::max()) {
    return reject<OperationAcceptedDTO>("operation identity space is exhausted",
                                        "operation_id_exhausted", ErrorCode::kInternalError);
  }
  const auto operation_id = OperationId("operation." + config_.request_owner_epoch.value() + "." +
                                        std::to_string(next_operation_id_));
  const OperationRevision operation_revision(1U);
  const auto accepted_at_ns = clock_.now();
  std::vector<PreparedAdmission> admissions;
  std::vector<OperationParticipantAssignmentDTO> assignments;
  std::vector<OperationParticipantStateDTO> participant_states;
  std::vector<ActiveOperationParticipant> active_participants;
  std::vector<ResourceSlotReservationOwnerDTO> reservation_rows;
  std::set<IResolvedParticipantExecutionEngine*> selected_engines;
  std::set<ResourceId> selected_resources;
  double coordinated_initial_runtime_scale = 1.0;
  std::uint64_t coordinated_initial_runtime_scale_sequence = 0U;
  const bool all_motion = std::all_of(
      intent.participants.begin(), intent.participants.end(), [](const auto& participant) {
        return std::holds_alternative<MotionParticipantIntentDTO>(participant.payload);
      });
  if (all_motion) {
    RuntimeScaleControlSelectorDTO selector{intent.program_binding_id, ControllableComponentId(""),
                                            CommandTargetId("")};
    for (const auto& participant : intent.participants) {
      selector.group_targets.push_back({participant.target.component_id, participant.target.command_target_id});
    }
    std::lock_guard state_lock(state_mutex_);
    auto scope = resolve_runtime_scale_control_locked(selector);
    if (!scope.ok()) return Result<OperationAcceptedDTO>::Err(scope.error());
    const auto setting =
        std::find_if(runtime_scale_control_settings_.begin(), runtime_scale_control_settings_.end(),
                     [&](const auto& candidate) {
                       return candidate.resolution.same_control_scope(scope.value());
                     });
    if (setting != runtime_scale_control_settings_.end())
      coordinated_initial_runtime_scale = setting->requested_scale;
    // Taken with the seed it orders (OD-72).
    coordinated_initial_runtime_scale_sequence = next_runtime_scale_value_sequence();
  }
  admissions.reserve(intent.participants.size());
  assignments.reserve(intent.participants.size());
  participant_states.reserve(intent.participants.size());
  active_participants.reserve(intent.participants.size());
  reservation_rows.reserve(intent.participants.size());

  for (std::size_t index = 0U; index < intent.participants.size(); ++index) {
    auto resolved = resolve_registration(intent, index);
    {
      std::lock_guard state_lock(state_mutex_);
      if (motion_admission_closed_locked())
        return reject<OperationAcceptedDTO>(
            "new motion admission is closed until the execution host restarts",
            "motion_admission_closed", ErrorCode::kInvalidStateTransition);
    }
    if (!resolved.ok()) {
      return Result<OperationAcceptedDTO>::Err(resolved.error());
    }
    auto& registration = resolved.value();
    if (registration.intrinsic_direct_control) {
      return reject<OperationAcceptedDTO>(
          "compiler-direct products admit one manual operation participant",
          "direct_control_coordination_unsupported", ErrorCode::kInvalidRequest);
    }
    if (registration.engine == nullptr || registration.prepared_motion_engine == nullptr ||
        registration.stop_execution_port == nullptr) {
      return reject<OperationAcceptedDTO>(
          "coordinated motion participant lacks its prepared or Stop execution port",
          "coordinated_participant_execution_port_missing", ErrorCode::kMissingReference);
    }
    if (!selected_engines.insert(registration.engine).second) {
      return reject<OperationAcceptedDTO>(
          "coordinated participants resolve to one ambiguous subordinate engine",
          "coordinated_participant_engine_ambiguous", ErrorCode::kInvalidRequest);
    }
    const auto payload_kind =
        std::holds_alternative<MotionParticipantIntentDTO>(intent.participants[index].payload)
            ? OperationParticipantPayloadKind::kMotion
            : OperationParticipantPayloadKind::kCapabilityCommand;
    if (!registration.stop_readiness.has_value() ||
        (payload_kind == OperationParticipantPayloadKind::kMotion &&
         !registration.participant_policy.has_value())) {
      return reject<OperationAcceptedDTO>(
          "coordinated participant lacks sealed readiness or motion policy",
          "coordinated_participant_authority_missing", ErrorCode::kMissingReference);
    }
    const auto& binding = registration.binding.value();
    if (!binding.control_path.has_value()) {
      return reject<OperationAcceptedDTO>("resolved participant has no active control path",
                                          "runtime_participant_control_path_missing",
                                          ErrorCode::kMissingReference);
    }
    const auto* control = &*binding.control_path;
    if (runtime_route_configuration_publication_required_.contains(control->control_domain_id)) {
      return reject<OperationAcceptedDTO>(
          "runtime route configuration has not been published after reconfiguration",
          "runtime_route_publication_required", ErrorCode::kInvalidStateTransition);
    }
    const auto route_readback =
        runtime_route_configuration_readbacks_.find(control->control_domain_id);
    if (route_readback != runtime_route_configuration_readbacks_.end() &&
        (!route_readback->second.operation_admission_open ||
         route_readback->second.active_configuration.active_control_configuration_id !=
             control->active_control_configuration_id ||
         route_readback->second.active_configuration.active_control_configuration_generation !=
             control->active_control_configuration_generation)) {
      return reject<OperationAcceptedDTO>("runtime route configuration is unreconciled or stale",
                                          "runtime_route_configuration_not_ready",
                                          ErrorCode::kInvalidStateTransition);
    }
    if (auto refusal = component_command_refusal_locked(registration); refusal.has_value()) {
      return Result<OperationAcceptedDTO>::Err(std::move(*refusal));
    }

    std::vector<ResourceId> resources;
    resources.reserve(binding.resources.size());
    for (const auto& resource : binding.resources) {
      if (!selected_resources.insert(resource.resource_id).second) {
        return reject<OperationAcceptedDTO>("coordinated participant resource scopes overlap",
                                            "coordinated_participant_resource_overlap",
                                            ErrorCode::kInvalidRequest);
      }
      resources.push_back(resource.resource_id);
    }
    const auto exact_identity = runtime_participant_binding_identity(binding);
    if (!exact_identity.ok()) return Result<OperationAcceptedDTO>::Err(exact_identity.error());

    const auto ordinal = index + 1U;
    const auto participant_id =
        OperationParticipantId(operation_id.value() + ".participant." + std::to_string(ordinal));
    const auto controller_goal_id =
        ControllerGoalId(operation_id.value() + ".controller-goal." + std::to_string(ordinal));
    const ControllerGoalGeneration controller_goal_generation(1U);
    const auto& participant_intent = intent.participants[index];
    std::optional<ExecuteRequestDTO> motion_request;
    std::optional<double> requested_tcp_linear_speed_mps;
    std::optional<GripperCommandRequestDTO> command_request;
    if (payload_kind == OperationParticipantPayloadKind::kMotion) {
      const auto& motion = std::get<MotionParticipantIntentDTO>(participant_intent.payload);
      requested_tcp_linear_speed_mps = motion.speed_profile.max_tcp_linear_speed_mps;
      if (binding.control_path.has_value() &&
          binding.control_path->runtime_scale_qualification.has_value() &&
          runtime_scale_availability_of(registration.engine) ==
              RuntimeScaleMechanismAvailability::kControllerNotListening) {
        return reject<OperationAcceptedDTO>(
            "a motion's controller is not listening for runtime scale input",
            kRuntimeScaleControllerNotListening, ErrorCode::kUnsupportedCapability);
      }
      double initial_runtime_scale = 1.0;
      std::uint64_t initial_runtime_scale_sequence = 0U;
      {
        std::lock_guard state_lock(state_mutex_);
        initial_runtime_scale = all_motion ? coordinated_initial_runtime_scale
                                           : runtime_scale_setting_for_binding_locked(binding);
        initial_runtime_scale_sequence = all_motion ? coordinated_initial_runtime_scale_sequence
                                                    : next_runtime_scale_value_sequence();
      }
      auto request =
          build_motion_request(intent, participant_intent, motion, binding, operation_id,
                               operation_revision, participant_id, controller_goal_id,
                               controller_goal_generation, registration, initial_runtime_scale,
                               initial_runtime_scale_sequence);
      if (!request.ok()) {
        return Result<OperationAcceptedDTO>::Err(request.error());
      }
      motion_request = std::move(request.value());
    } else {
      auto request = build_command_request(
          std::get<CapabilityCommandIntentDTO>(participant_intent.payload), binding,
          registration.engine->gripper_actuation_kind(binding.command_target_id.value()),
          registration.engine->gripper_output_mode(binding.command_target_id.value()));
      if (!request.ok()) {
        return Result<OperationAcceptedDTO>::Err(request.error());
      }
      auto private_validation = registration.engine->validate_gripper_command(request.value());
      if (!private_validation.ok()) {
        return Result<OperationAcceptedDTO>::Err(private_validation.error());
      }
      command_request = std::move(request.value());
    }

    std::shared_ptr<const PreparedParticipantStopPlanDTO> retained_stop_plan;
    std::shared_ptr<const PreparedParticipantStopPlanDTO> retained_fault_stop_plan;
    std::shared_ptr<const PreparedOperationParticipantDTO> prepared_command_product;
    {
      PreparedParticipantStopPlanContextDTO stop_context{
          operation_id,
          operation_revision,
          participant_id,
          intent.program_id,
          intent.program_revision,
          intent.program_binding_id,
          intent.program_run_id,
          payload_kind == OperationParticipantPayloadKind::kMotion
              ? registration.default_operator_stop_profile
              : std::optional<MotionInterruptStopProfileDTO>{},
          payload_kind == OperationParticipantPayloadKind::kMotion &&
                  registration.default_operator_stop_profile.has_value()
              ? ParticipantStopProfilePurposeDTO::kOperator
              : ParticipantStopProfilePurposeDTO::kOrdinary,
          std::nullopt,
          registration.stop_node_category};
      auto stop_plan = MakePreparedParticipantStopPlan(
          registration.stop_readiness->value().resolved_stop_base_plan, std::move(stop_context),
          binding.resources, registration.retained_default_stop_limits);
      if (!stop_plan.ok()) {
        return Result<OperationAcceptedDTO>::Err(stop_plan.error());
      }
      auto fault_stop_plan = make_runtime_fault_stop_plan(
          *registration.stop_readiness,
          payload_kind == OperationParticipantPayloadKind::kMotion
              ? registration.default_fault_stop_profile
              : std::optional<MotionInterruptStopProfileDTO>{},
          binding, registration.retained_default_stop_limits, registration.stop_node_category,
          operation_id, operation_revision, participant_id, intent.program_id,
          intent.program_revision, intent.program_binding_id, intent.program_run_id);
      if (!fault_stop_plan.ok()) {
        return Result<OperationAcceptedDTO>::Err(fault_stop_plan.error());
      }
      try {
        retained_stop_plan =
            std::make_shared<const PreparedParticipantStopPlanDTO>(std::move(stop_plan.value()));
        if (fault_stop_plan.value().has_value()) {
          retained_fault_stop_plan = std::make_shared<const PreparedParticipantStopPlanDTO>(
              std::move(*fault_stop_plan.value()));
        }
        if (payload_kind == OperationParticipantPayloadKind::kCapabilityCommand) {
          auto prepared = build_prepared_capability_command_participant(
              intent, std::get<CapabilityCommandIntentDTO>(participant_intent.payload),
              registration.binding, *registration.stop_readiness,
              registration.retained_default_stop_limits, registration.stop_node_category,
              operation_id, operation_revision, participant_id, *retained_stop_plan);
          if (!prepared.ok()) {
            return Result<OperationAcceptedDTO>::Err(prepared.error());
          }
          prepared_command_product =
              std::make_shared<const PreparedOperationParticipantDTO>(std::move(prepared.value()));
        }
      } catch (...) {
        return reject<OperationAcceptedDTO>(
            "the coordinated Stop plan could not be retained before reservation",
            "operation_admission_capacity_unavailable", ErrorCode::kInternalError);
      }
    }
    ResourceSlotOwnerDTO slot_owner{operation_id,
                                    operation_revision,
                                    participant_id,
                                    control->control_domain_id,
                                    control->control_domain_revision,
                                    control->control_binding_id,
                                    control->control_binding_revision,
                                    control->controller_topology_id,
                                    control->topology_revision,
                                    control->control_route_id,
                                    control->active_control_configuration_id,
                                    control->active_control_configuration_generation,
                                    control->containment_granularity};
    ResourceSlotReservationOwnerDTO reservation{slot_owner, resources};
    assignments.push_back(OperationParticipantAssignmentDTO{index, participant_id});
    participant_states.push_back(OperationParticipantStateDTO{
        participant_id, controller_goal_id, controller_goal_generation,
        binding.controllable_component_id, binding.command_target_id, binding.capability_id,
        exact_identity.value(), payload_kind, OperationStateKind::kResolving, 0.0, resources,
        std::nullopt, participant_intent.execution_scope});
    active_participants.push_back(
        ActiveOperationParticipant{registration.binding,
                                   registration.stop_readiness,
                                   registration.retained_default_stop_limits,
                                   registration.stop_node_category,
                                   registration.default_operator_stop_profile,
                                   retained_stop_plan,
                                   registration.default_fault_stop_profile,
                                   retained_fault_stop_plan,
                                   std::nullopt,
                                   registration.engine,
                                   registration.prepared_motion_engine,
                                   registration.stop_execution_port,
                                   slot_owner,
                                   resources,
                                   payload_kind,
                                   ResourceSlotState::kReserved,
                                   0U,
                                   0U,
                                   0U,
                                   false,
                                   false,
                                   false,
                                   false,
                                   false,
                                   false,
                                   std::nullopt,
                                   motion_request});
    active_participants.back().engine_subscription_id = registration.engine_subscription_id;
    active_participants.back().io_motion_rule_policies = registration.io_motion_rule_policies;
    if (payload_kind == OperationParticipantPayloadKind::kCapabilityCommand)
      active_participants.back().gripper_actuation =
          registration.engine->gripper_actuation_kind(binding.command_target_id.value());
    reservation_rows.push_back(reservation);
    admissions.push_back(
        PreparedAdmission{std::move(registration), payload_kind, std::move(motion_request),
                          requested_tcp_linear_speed_mps, std::move(command_request),
                          std::move(prepared_command_product), std::move(retained_stop_plan),
                          std::move(retained_fault_stop_plan)});
  }

  // Synchronized multi-motion execution is withdrawn. Keep the
  // existing resolved route/readiness/model checks, then refuse centrally
  // before admission; participant adapters cannot turn it into independent motion.
  if (std::count_if(admissions.begin(), admissions.end(), [](const auto& admission) {
        return admission.motion_request.has_value();
      }) > 1) {
    return reject<OperationAcceptedDTO>("coordinated multi-motion execution is not supported",
                                        "coordinated_multi_motion_unsupported",
                                        ErrorCode::kUnsupportedCapability);
  }

  if (std::any_of(admissions.begin(), admissions.end(), [](const auto& admission) {
        return admission.motion_request.has_value() &&
               !admission.registration.default_fault_stop_profile.has_value() &&
               controller_goal_cancel_stage_index(*admission.stop_plan) ==
                   admission.stop_plan->stage_commands.size();
      })) {
    return reject<OperationAcceptedDTO>(
        "motion without a fault Stop profile requires a qualified controller-goal cancel",
        "runtime_collision_cancel_unqualified", ErrorCode::kUnsupportedCapability);
  }

  std::sort(reservation_rows.begin(), reservation_rows.end(),
            [](const auto& left, const auto& right) {
              return std::tie(left.owner.operation_id, left.owner.participant_id) <
                     std::tie(right.owner.operation_id, right.owner.participant_id);
            });
  const OperationAcceptedDTO accepted{intent.request_context.request_id,
                                      RequestReplayStatusDTO::kAcceptedNew,
                                      operation_id,
                                      operation_revision,
                                      assignments,
                                      accepted_at_ns};
  OperationStateDTO operation_state{operation_id,
                                    operation_revision,
                                    intent.program_binding_id,
                                    intent.program_run_id,
                                    intent.coordination_policy,
                                    OperationStateKind::kResolving,
                                    std::move(participant_states),
                                    accepted_at_ns,
                                    std::nullopt,
                                    std::nullopt,
                                    std::nullopt};
  std::map<OperationId, std::unique_ptr<ActiveOperation>>::node_type prepared_operation;
  ReplayEntry prepared_replay{intent, accepted};
  std::vector<OperationId> protected_operation_ids;
  try {
    std::optional<RuntimeScaleStatusDTO> initial_runtime_scale_status;
    const bool all_scalable =
        std::all_of(admissions.begin(), admissions.end(), [](const auto& item) {
          return item.motion_request && item.motion_request->initial_runtime_scale;
        });
    if (all_scalable) {
      const double scale =
          admissions.front().motion_request->initial_runtime_scale->requested_scale;
      initial_runtime_scale_status = RuntimeScaleStatusDTO{operation_id,
                                                           operation_revision,
                                                           RuntimeScaleGeneration(0U),
                                                           scale,
                                                           scale,
                                                           {},
                                                           RuntimeScaleApplicationState::kPending,
                                                           accepted_at_ns};
      for (const auto& admission : admissions)
        initial_runtime_scale_status->participants.push_back(initial_scale_status(
            *admission.motion_request->initial_runtime_scale,
            admission.registration.binding.value().control_path->provider_runtime_epoch));
      operation_state.runtime_scale_generation = RuntimeScaleGeneration(0U);
    }
    std::map<OperationId, std::unique_ptr<ActiveOperation>> staging;
    staging.emplace(operation_id, std::make_unique<ActiveOperation>(ActiveOperation{
                                      intent, accepted, std::move(active_participants),
                                      operation_state, std::nullopt, false, true, false,
                                      false, std::move(initial_runtime_scale_status)}));
    stamp_operation_capture_locked(*staging.at(operation_id), operation_state);
    staging.at(operation_id)->state = operation_state;
    prepared_operation = staging.extract(operation_id);
  } catch (...) {
    return reject<OperationAcceptedDTO>(
        "the coordinated operation state could not be retained before reservation",
        "operation_admission_capacity_unavailable", ErrorCode::kInternalError);
  }

  {
    // Match single-participant admission: ordering precedes Kernel state so a
    // blocked durable claim cannot convoy recognized Stop.
    std::lock_guard admission_lock(*admission_mutex_);
    std::lock_guard state_lock(state_mutex_);
    for (const auto& admission : admissions) {
      if (auto refusal = component_command_refusal_locked(admission.registration)) {
        return Result<OperationAcceptedDTO>::Err(std::move(*refusal));
      }
    }
    if (motion_admission_closed_locked()) {
      return reject<OperationAcceptedDTO>(
          "new motion admission closed while the operation was being prepared",
          "motion_admission_closed", ErrorCode::kInvalidStateTransition);
    }
    if (const auto scope = validate_program_execution_scope_locked(
            intent, std::all_of(prepared_operation.mapped()->participants.begin(),
                                prepared_operation.mapped()->participants.end(),
                                [](const auto& row) { return row.non_motion_transaction(); }));
        !scope.ok()) {
      return Result<OperationAcceptedDTO>::Err(scope.error());
    }
    // One intersecting participant rejects the complete coordinated admission.
    if (auto refusal = operator_alarm_attention_refusal_locked(
            std::vector<ResourceId>(selected_resources.begin(), selected_resources.end()))) {
      return Result<OperationAcceptedDTO>::Err(std::move(*refusal));
    }
    if (program_cycle_claim_run_ && program_cycle_claim_run_->capture &&
        program_cycle_claim_run_->program_run_id == intent.program_run_id) {
      const auto& nodes = program_cycle_claim_run_->capture->table->nodes;
      for (std::size_t index = 0U; index < intent.participants.size(); ++index) {
        const auto& scope = *intent.participants[index].execution_scope;
        const auto node =
            std::lower_bound(nodes.begin(), nodes.end(), scope.node_instance_id,
                             [](const auto& row, const auto& id) { return row.instance_id < id; });
        const auto node_index = static_cast<std::size_t>(node - nodes.begin());
        if (node_index >= program_cycle_claim_run_->motion_continuations.size()) continue;
        const auto& pending = program_cycle_claim_run_->motion_continuations[node_index];
        if (!pending) continue;
        const auto& admission = admissions[index];
        if (node == nodes.end() || node->owner_api != "kernel.operation" ||
            pending->origin.node_instance_id != scope.node_instance_id ||
            pending->origin.node_attempt_id == scope.node_attempt_id ||
            admission.payload_kind != OperationParticipantPayloadKind::kMotion ||
            pending->binding.value() != admission.registration.binding.value() ||
            !admission.motion_request ||
            pending->base_frame != admission.motion_request->binding.base_frame ||
            pending->tcp_frame != admission.motion_request->binding.tcp_frame ||
            pending->planner_capability !=
                admission.motion_request->binding.planner_capability_binding ||
            pending->authored_motion_kind !=
                std::get<MotionParticipantIntentDTO>(intent.participants[index].payload)
                    .motion.index()) {
          return reject<OperationAcceptedDTO>(
              "new synchronized attempt differs from its retained motion continuation",
              "program_motion_continuation_mismatch", ErrorCode::kInvalidRequest);
        }
        auto& participant = prepared_operation.mapped()->participants[index];
        participant.continuation = pending->intent;
        participant.continuation_node_index = node_index;
      }
    }
    if (emergency_stop_reserve_.find(intent.request_context.request_id.value()).has_value()) {
      return Result<OperationAcceptedDTO>::Err(
          error("request_id_conflict", "operation request ID became retained by emergency Stop",
                "request_id_conflict"));
    }
    const bool engine_in_use =
        std::any_of(selected_engines.begin(), selected_engines.end(),
                    [&](auto* engine) { return immediate_step_engines_.contains(engine); }) ||
        std::any_of(active_operations_.begin(), active_operations_.end(), [&](const auto& item) {
          return std::any_of(
              selected_engines.begin(), selected_engines.end(),
              [&](const auto* engine) { return item.second->participant_for(engine) != nullptr; });
        });
    const bool request_id_used =
        replay_ledger_.contains(intent.request_context.request_id) ||
        control_replay_ledger_.contains(intent.request_context.request_id) ||
        stop_replay_ledger_.contains(intent.request_context.request_id) ||
        runtime_scale_control_replay_ledger_.contains(intent.request_context.request_id);
    if (request_id_used) {
      return Result<OperationAcceptedDTO>::Err(
          error("request_id_conflict", "operation request ID was used during admission",
                "request_id_conflict"));
    }
    if (engine_in_use) {
      return reject<OperationAcceptedDTO>(
          "one coordinated participant engine became busy during admission",
          "participant_engine_busy");
    }
    if (!make_operation_admission_capacity_locked()) {
      return Result<OperationAcceptedDTO>::Err(
          error("operation_capacity_full", "active operation capacity reached its fixed bound",
                "operation_capacity_full"));
    }
    auto reservation =
        config_.operation_admission_gate != nullptr
            ? config_.operation_admission_gate->reserve_batch(*resource_slots_, reservation_rows)
            : resource_slots_->reserve_batch(reservation_rows);
    if (!reservation.ok()) {
      return Result<OperationAcceptedDTO>::Err(reservation.error());
    }
    const auto inserted = active_operations_.insert(std::move(prepared_operation));
    if (!inserted.inserted) {
      const auto released = resource_slots_->release_batch(reservation_rows);
      if (!released.ok()) {
        return Result<OperationAcceptedDTO>::Err(released.error());
      }
      return reject<OperationAcceptedDTO>("operation identity is already resident",
                                          "operation_id_conflict", ErrorCode::kInvariantViolation);
    }
    ++next_operation_id_;
    protected_operation_ids = durable_operation_retention_ids_locked();
  }

  OperationCommitGuard admission_commit(*this, operation_id);
  auto durable_acceptance = attempt_journal_write([&] {
    return journal_.RecordOperationAccepted(config_.request_owner_epoch, intent, accepted,
                                            operation_state, protected_operation_ids);
  });
  if (!durable_acceptance.ok()) {
    std::lock_guard state_lock(state_mutex_);
    retain_operation_journal_write_fault_locked("operation_accepted", durable_acceptance.error(),
                                                &operation_state,
                                                intent.request_context.request_id);
    auto* operation = find_active_operation_locked(operation_id);
    if (operation != nullptr) {
      operation->ordinary_acceptance_pending = false;
      if (!operation->state.active_stop_id.has_value()) {
        const auto released = resource_slots_->release_batch(reservation_rows);
        if (!released.ok()) {
          return Result<OperationAcceptedDTO>::Err(released.error());
        }
        active_operations_.erase(operation_id);
      }
    }
    return Result<OperationAcceptedDTO>::Err(durable_acceptance.error());
  }

  bool prepare_allowed = false;
  {
    std::lock_guard state_lock(state_mutex_);
    auto* operation = find_active_operation_locked(operation_id);
    if (operation == nullptr) {
      return reject<OperationAcceptedDTO>("operation disappeared after durable acceptance",
                                          "active_operation_lost", ErrorCode::kInvariantViolation);
    }
    operation->ordinary_acceptance_pending = false;
    operation->ordinary_acceptance_durable = true;
    replay_ledger_.push_back(std::move(prepared_replay));
    prepare_allowed = std::all_of(operation->participants.begin(), operation->participants.end(),
                                  [](const auto& participant) {
                                    return !participant.stop_owns_reconciliation() &&
                                           participant.slot_state == ResourceSlotState::kReserved;
                                  });
  }
  admission_commit.release();
  // The coordinated operation is resident, durable and replayable: release
  // submission ordering before any participant Prepare or dispatch (OD-67).
  // No setting can miss it: multi-motion is refused above, a mixed operation
  // has no runtime_scale_status, and its scale control reports frozen
  // (runtime_scale_group_control_required).
  end_admission();
  if (!prepare_allowed) {
    const auto discarded = discard_uncommitted_ordinary_preparations(operation_id);
    if (!discarded.ok()) return Result<OperationAcceptedDTO>::Err(discarded.error());
    return Result<OperationAcceptedDTO>::Ok(accepted);
  }

  for (auto& admission : admissions) {
    std::shared_ptr<const detail::MotionResumeIntent> continuation;
    {
      std::lock_guard state_lock(state_mutex_);
      auto* operation = find_active_operation_locked(operation_id);
      auto* participant = operation == nullptr
                              ? nullptr
                              : operation->participant_for(admission.registration.engine);
      if (participant == nullptr) {
        return Result<OperationAcceptedDTO>::Ok(accepted);
      }
      if (participant->stop_owns_reconciliation() || participant->stop_cancelled_pre_dispatch ||
          terminal(operation->state.state)) {
        break;  // Dispose remaining ordinary members after leaving state ordering.
      }
      participant->prepare_in_flight = true;
      continuation = participant->continuation;
    }
    Result<std::uint64_t> prepared = [&]() {
      if (admission.payload_kind == OperationParticipantPayloadKind::kMotion) {
        ResolvedPreparedMotionExecutionRequestDTO authority{
            *admission.motion_request,
            operation_revision,
            admission.registration.binding,
            *admission.registration.stop_readiness,
            admission.requested_tcp_linear_speed_mps,
            *admission.registration.deployment_maximum_tcp_linear_speed_mps,
            admission.registration.retained_default_stop_limits,
            admission.registration.stop_node_category,
            admission.stop_plan,
            admission.fault_stop_plan,
            std::move(continuation)};
        for (const auto& scope : admission.motion_request->motion_interrupt_scopes) {
          const auto policy = std::find_if(
              admission.registration.io_motion_rule_policies.begin(),
              admission.registration.io_motion_rule_policies.end(),
              [&](const auto& value) { return value.policy_id.value() == scope.policy_id; });
          if (policy == admission.registration.io_motion_rule_policies.end() || !policy->semantics)
            return reject<std::uint64_t>("prepared motion lost its admitted interrupt action",
                                         "motion_interrupt_action_not_admitted",
                                         ErrorCode::kInvalidRequest);
          authority.interrupt_scope_actions.push_back(
              {MotionInterruptScopeId(scope.scope_id), policy->semantics->preemption_action,
               policy->stop_entry_stage_id, policy->success_through_stage_id});
        }
        return admission.registration.prepared_motion_engine->PrepareMotion(authority);
      }
      return admission.registration.prepared_motion_engine->PrepareCapabilityCommand(
          ResolvedPreparedCapabilityCommandExecutionRequestDTO{
              admission.prepared_command_product, admission.registration.binding,
              *admission.registration.stop_readiness,
              admission.registration.retained_default_stop_limits,
              admission.registration.stop_node_category, *admission.command_request});
    }();
    if (!prepared.ok()) {
      {
        // This exact initial Prepare call has returned without a product.
        // Retire its borrow even when a sibling already owns the abort.
        std::lock_guard state_lock(state_mutex_);
        auto* operation = find_active_operation_locked(operation_id);
        if (auto* participant = operation == nullptr
                                    ? nullptr
                                    : operation->participant_for(admission.registration.engine)) {
          participant->last_subordinate_error = prepared.error();
          participant->prepare_in_flight = false;
          settle_completed_control_locked(operation_id);
          wake_stop_after_preparation_borrow_locked(operation_id);
        }
      }
      (void)fail_pre_dispatch(operation_id, admission.registration.engine, prepared.error());
      return Result<OperationAcceptedDTO>::Ok(accepted);
    }

    Result<void> admitted = Result<void>::Ok();
    bool preparation_orphaned = false;
    bool stop_owns_preparation = false;
    bool cancel_late_preparation = false;
    std::optional<StopControlWorkToken> stop_wakeup;
    {
      OperationCommitGuard commit(*this, operation_id);
      std::unique_lock state_lock(state_mutex_);
      auto* operation = find_active_operation_locked(operation_id);
      auto* participant = operation == nullptr
                              ? nullptr
                              : operation->participant_for(admission.registration.engine);
      if (participant == nullptr) {
        preparation_orphaned = true;
        admitted = reject<void>("coordinated participant disappeared during preparation",
                                "active_operation_lost", ErrorCode::kInvariantViolation);
      } else {
        // An abort captured while this call was still in flight did not own
        // its returned product. Keep the borrow until that exact product has
        // been cancelled, or handed to an actual Stop transaction.
        cancel_late_preparation = participant->stop_cancelled_pre_dispatch &&
                                  (!operation->state.active_stop_id.has_value() ||
                                   operation->terminal_stop_result().has_value());
        participant->prepare_in_flight = cancel_late_preparation;
        if (participant->subordinate_id != 0U && participant->subordinate_id != prepared.value()) {
          admitted =
              reject<void>("prepared result identity changed during coordinated admission",
                           "subordinate_execution_id_mismatch", ErrorCode::kInvariantViolation);
        } else {
          participant->subordinate_id = prepared.value();
          participant->preparation_id = prepared.value();
          const auto state_row =
              std::find_if(operation->state.participants.begin(),
                           operation->state.participants.end(), [&](const auto& row) {
                             return row.participant_id == participant->slot_owner.participant_id;
                           });
          if (state_row == operation->state.participants.end()) {
            admitted =
                reject<void>("coordinated participant state row is missing",
                             "operation_participant_state_missing", ErrorCode::kInvariantViolation);
          } else if (participant->stop_owns_reconciliation() &&
                     operation->state.active_stop_id.has_value() &&
                     operation->state.active_stop_generation.has_value()) {
            stop_owns_preparation = !cancel_late_preparation;
            const auto transaction = std::find_if(
                stop_transactions_.begin(), stop_transactions_.end(), [&](const auto& candidate) {
                  return candidate != nullptr && candidate->active &&
                         candidate->stop_id == operation->state.active_stop_id &&
                         candidate->stop_generation == *operation->state.active_stop_generation;
                });
            if (transaction != stop_transactions_.end() && !(*transaction)->terminal) {
              stop_wakeup = (*transaction)->token;
              stop_control_executor_->ArmDeadline(*stop_wakeup, clock_.now());
            }
          } else if (!cancel_late_preparation) {
            auto captured = operation->state;
            captured.participants[std::distance(operation->state.participants.begin(), state_row)]
                .state = admission.payload_kind == OperationParticipantPayloadKind::kMotion
                             ? OperationStateKind::kPlanning
                             : OperationStateKind::kPrepared;
            captured.state = aggregate_participant_state(captured.participants);
            captured.updated_at_ns = clock_.now();
            auto durable = record_operation_state_locked(
                state_lock, captured, nullptr,
                admission.payload_kind == OperationParticipantPayloadKind::kMotion
                    ? "coordinated_participant_motion_admitted"
                    : "coordinated_participant_command_admitted");
            if (!durable.ok()) {
              admitted = Result<void>::Err(durable.error());
            } else if (durable.value()) {
              operation->state = std::move(captured);
            } else {
              stop_owns_preparation = true;
            }
          }
        }
      }
    }
    if (stop_wakeup.has_value()) {
      stop_control_executor_->Publish(*stop_wakeup);
    }
    if (stop_owns_preparation) {
      const auto discarded = discard_uncommitted_ordinary_preparations(operation_id);
      if (!discarded.ok()) return Result<OperationAcceptedDTO>::Err(discarded.error());
      return Result<OperationAcceptedDTO>::Ok(accepted);
    }
    if (cancel_late_preparation) {
      const auto cancelled = cancel_prepared_participant(
          *admission.registration.prepared_motion_engine, admission.payload_kind, prepared.value());
      OperationCommitGuard commit(*this, operation_id);
      std::unique_lock state_lock(state_mutex_);
      auto* operation = find_active_operation_locked(operation_id);
      auto* participant = operation == nullptr
                              ? nullptr
                              : operation->participant_for(admission.registration.engine);
      if (participant == nullptr) {
        return Result<OperationAcceptedDTO>::Ok(accepted);
      }
      participant->prepare_in_flight = false;
      if (!cancelled.ok()) {
        participant->last_subordinate_error = cancelled.error();
        for (auto& row : operation->state.participants) {
          if (row.participant_id == participant->slot_owner.participant_id) {
            row.state = OperationStateKind::kInterventionRequired;
            row.terminal_error = cancelled.error();
          }
        }
        operation->state.state = OperationStateKind::kInterventionRequired;
        operation->state.updated_at_ns = clock_.now();
        stamp_operation_capture_locked(*operation, operation->state);
        if (!operation->state.active_stop_id.has_value()) {
          auto captured = operation->state;
          auto durable = record_operation_state_locked(state_lock, captured, nullptr,
                                                       "coordinated_late_prepare_cancel_failed");
          if (durable.ok() && durable.value()) operation->state = std::move(captured);
        }
      }
      // This path follows an abort or an already terminal Stop. Disposal
      // releases the borrowed product; there is no active Stop to wake.
      return Result<OperationAcceptedDTO>::Ok(accepted);
    }
    if (preparation_orphaned) {
      static_cast<void>(cancel_prepared_participant(
          *admission.registration.prepared_motion_engine, admission.payload_kind, prepared.value()));
      return Result<OperationAcceptedDTO>::Ok(accepted);
    }
    if (!admitted.ok()) {
      (void)fail_pre_dispatch(operation_id, admission.registration.engine, admitted.error());
      return Result<OperationAcceptedDTO>::Ok(accepted);
    }
    auto advanced =
        admission.payload_kind == OperationParticipantPayloadKind::kMotion
            ? advance_prepared_motion(operation_id, admission.registration.engine)
            : advance_prepared_capability_command(operation_id, admission.registration.engine);
    if (!advanced.ok()) {
      (void)fail_pre_dispatch(operation_id, admission.registration.engine, advanced.error());
      return Result<OperationAcceptedDTO>::Ok(accepted);
    }
  }
  const auto discarded = discard_uncommitted_ordinary_preparations(operation_id);
  if (!discarded.ok()) return Result<OperationAcceptedDTO>::Err(discarded.error());
  return Result<OperationAcceptedDTO>::Ok(accepted);
}

Result<OperationControlAcceptedDTO> ResourceAddressedExecutionKernel::reset(
    const OperationControlRequestDTO& request) {
  return apply_control(request, OperationControlKind::kReset);
}

Result<OperationControlAcceptedDTO> ResourceAddressedExecutionKernel::acknowledge_completed(
    const OperationControlRequestDTO& request) {
  return apply_control(request, OperationControlKind::kAcknowledgeCompleted);
}

Result<OperationControlAcceptedDTO> ResourceAddressedExecutionKernel::apply_control(
    const OperationControlRequestDTO& request, OperationControlKind control,
    const bool automatic_manual_retirement) {
  struct ControlTarget final {
    OperationParticipantId participant_id;
    IResolvedParticipantExecutionEngine* engine{};
    std::uint64_t subordinate_id{};
    OperationParticipantPayloadKind payload_kind{OperationParticipantPayloadKind::kMotion};
    std::string command_target;
    std::uint64_t subordinate_generation{};
  };
  if (operation_control_request_field_error(request).has_value()) {
    return reject<OperationControlAcceptedDTO>("operation control request is malformed",
                                               "operation_control_request_invalid",
                                               ErrorCode::kInvalidRequest);
  }
  if (request.request_context.request_owner_epoch != config_.request_owner_epoch) {
    return Result<OperationControlAcceptedDTO>::Err(
        error("stale_request_owner_epoch", "operation control names a stale Kernel owner epoch",
              "stale_request_owner_epoch"));
  }

  OperationCommitGuard commit(*this, request.operation_id);
  std::vector<ControlTarget> targets;
  bool reset_already_terminal = false;
  bool no_effect_preparation_retirement = false;
  std::optional<OperationStateDTO> accepted_state;
  OperationControlAcceptedDTO accepted{RequestId("pending"),
                                       RequestReplayStatusDTO::kAcceptedNew,
                                       OperationId("pending"),
                                       OperationRevision(0U),
                                       control,
                                       OperationRevision(0U),
                                       0};
  {
    std::lock_guard state_lock(state_mutex_);
    if (operation_control_reconfiguration_in_progress_) {
      return reject<OperationControlAcceptedDTO>(
          "operation-control lanes are being atomically reconfigured",
          "operation_control_reconfiguration_in_progress", ErrorCode::kInvalidStateTransition);
    }
    if (emergency_stop_reserve_.find(request.request_context.request_id.value()).has_value()) {
      return Result<OperationControlAcceptedDTO>::Err(
          error("request_id_conflict", "operation control request ID is retained by emergency Stop",
                "request_id_conflict"));
    }
    const auto replay = control_replay_ledger_.find(request.request_context.request_id);
    if (replay != control_replay_ledger_.end()) {
      if (replay->request != request || replay->accepted.control != control) {
        return Result<OperationControlAcceptedDTO>::Err(error(
            "request_id_conflict", "operation control request ID was reused with different intent",
            "request_id_conflict"));
      }
      if (!replay->durable) {
        return reject<OperationControlAcceptedDTO>(
            "operation control acceptance is still being durably admitted",
            "operation_control_admission_pending", ErrorCode::kInvalidStateTransition);
      }
      auto accepted = replay->accepted;
      accepted.replay_status = RequestReplayStatusDTO::kReplayedExact;
      return Result<OperationControlAcceptedDTO>::Ok(std::move(accepted));
    }
    if (replay_ledger_.contains(request.request_context.request_id)) {
      return Result<OperationControlAcceptedDTO>::Err(
          error("request_id_conflict", "operation control request ID was already used by Execute",
                "request_id_conflict"));
    }
    if (stop_replay_ledger_.contains(request.request_context.request_id) ||
        runtime_scale_control_replay_ledger_.contains(request.request_context.request_id)) {
      return Result<OperationControlAcceptedDTO>::Err(
          error("request_id_conflict",
                "operation control request ID was already used by another Kernel command",
                "request_id_conflict"));
    }
    auto* active_operation = find_active_operation_locked(request.operation_id);
    if (active_operation == nullptr) {
      return reject<OperationControlAcceptedDTO>("operation control names a non-resident operation",
                                                 "operation_not_resident",
                                                 ErrorCode::kMissingReference);
    }
    if (automatic_manual_retirement &&
        (!manual_operation_claim_.has_value() ||
         manual_operation_claim_->intent.owner_epoch != config_.request_owner_epoch ||
         manual_operation_claim_->intent.owner_transaction_id != request.operation_id.value() ||
         (active_operation->state.state != OperationStateKind::kCompleted &&
          active_operation->state.state != OperationStateKind::kStopped &&
          active_operation->state.state != OperationStateKind::kCancelledPreDispatch) ||
         // The Program-requested Stop's typed cause is no fault to hold.
         (!active_operation->ordinary_program_stop() &&
          std::any_of(active_operation->state.participants.begin(),
                      active_operation->state.participants.end(),
                      [](const auto& row) { return row.terminal_error.has_value(); })))) {
      return reject<OperationControlAcceptedDTO>(
          "automatic retirement requires current clean manual settlement",
          "manual_operation_not_clean_terminal");
    }
    // Like Stop, control resolves to the operation's current revision; the
    // caller's expected revision is informational.
    const auto current_revision = active_operation->state.operation_revision;
    if (!control_admissible(control, active_operation->state.state) &&
        !(control == OperationControlKind::kAcknowledgeCompleted &&
          active_operation->pneumatic_final_failure())) {
      return reject<OperationControlAcceptedDTO>(
          std::string(control_name(control)) + " is invalid for the current operation state",
          "operation_control_state_invalid", ErrorCode::kInvalidStateTransition);
    }
    if (active_operation->pending_control.has_value() ||
        active_operation->recovery_effect_in_flight ||
        active_operation->coordinated_cancellation_in_flight) {
      return reject<OperationControlAcceptedDTO>(
          "another operation control is already being admitted", "operation_control_busy");
    }
    if ((control == OperationControlKind::kAcknowledgeCompleted ||
         control == OperationControlKind::kReset) &&
        (active_operation->has_pending_io_rule_response() ||
         active_operation->dispatch_effect_in_flight() ||
         active_operation->reconciliation_in_flight() ||
         active_operation->stop_effect_in_flight ||
         (terminal(active_operation->state.state) &&
          std::any_of(active_operation->participants.begin(), active_operation->participants.end(),
                      [](const auto& participant) {
                        return participant.preparation_borrow_in_flight();
                      })))) {
      return reject<OperationControlAcceptedDTO>(
          "operation settlement cannot overtake an in-flight subordinate effect",
          "operation_effect_in_flight");
    }
    if ((control == OperationControlKind::kAcknowledgeCompleted ||
         control == OperationControlKind::kReset) &&
        active_operation->state.active_stop_id.has_value() &&
        !active_operation->terminal_stop_result().has_value()) {
      // A participant may already be at rest while the containing Stop is
      // still settling another participant. Its exact certificate, not that
      // early local observation, is the existing cleanup authority.
      return reject<OperationControlAcceptedDTO>(
          "operation settlement awaits its exact Stop certificate", "operation_effect_in_flight");
    }
    if (control == OperationControlKind::kReset &&
        active_operation->cancelled_pneumatic_preparation_shape()) {
      if (!active_operation->permanent_preparation_no_effect())
        return reject<OperationControlAcceptedDTO>(
            "cancelled preparation still lacks exact returned-owner no-effect proof", "operation_effect_in_flight");
      const auto retained = retained_owner_rows(active_operation->participants, resource_slots_->coherent_snapshot());
      if (!retained.ok()) return Result<OperationControlAcceptedDTO>::Err(retained.error());
      if (retained.value().size() != active_operation->participants.size())
        return reject<OperationControlAcceptedDTO>("cancelled preparation lost its exact resources", "resource_owner_mismatch");
      no_effect_preparation_retirement = true;
    }
    if (control == OperationControlKind::kReset) {
      for (const auto& participant : active_operation->participants)
        if (participant.non_motion_transaction() && participant.stop_cancelled_pre_dispatch &&
            !participant.permanent_preparation_no_effect())
          return reject<OperationControlAcceptedDTO>(
              "cancelled command still lacks permanent returned-owner no-effect proof",
              "operation_effect_in_flight");
    }
    reset_already_terminal = terminal(active_operation->state.state);
    if (current_revision.value() >= kMaximumKernelJsonInteger) {
      return reject<OperationControlAcceptedDTO>("operation revision space is exhausted",
                                                 "operation_revision_exhausted",
                                                 ErrorCode::kInternalError);
    }
    const OperationRevision next_revision(current_revision.value() + 1U);
    targets.reserve(active_operation->participants.size());
    for (std::size_t index = 0U; index < active_operation->participants.size(); ++index) {
      const auto& participant = active_operation->participants[index];
      const auto& participant_state = active_operation->state.participants[index];
      if (control == OperationControlKind::kAcknowledgeCompleted &&
          (participant.payload_kind == OperationParticipantPayloadKind::kMotion ||
           participant.finishes_current_gripper_transaction())) {
        targets.push_back(ControlTarget{participant_state.participant_id,
                                        participant.engine,
                                        participant.finishes_current_gripper_transaction()
                                            ? participant.preparation_id : participant.subordinate_id,
                                        participant.payload_kind,
                                        participant.binding.value().command_target_id.value(),
                                        participant.subordinate_generation});
      } else if (control == OperationControlKind::kReset && !no_effect_preparation_retirement &&
                 !participant.permanent_preparation_no_effect() && participant.subordinate_id != 0U &&
                 (participant.prepared_dispatch_released ||
                  (participant.payload_kind == OperationParticipantPayloadKind::kMotion &&
                   participant.stop_cancelled_pre_dispatch &&
                   active_operation->state.active_stop_id.has_value()))) {
        targets.push_back(ControlTarget{participant_state.participant_id,
                                        participant.engine,
                                        participant.subordinate_id,
                                        participant.payload_kind,
                                        participant.binding.value().command_target_id.value(),
                                        participant.subordinate_generation});
      }
    }
    if (no_effect_preparation_retirement && !targets.empty())
      return reject<OperationControlAcceptedDTO>("no-effect retirement cannot contain provider targets",
                                                "operation_effect_in_flight");
    const auto accepted_at_ns = clock_.now();
    accepted = OperationControlAcceptedDTO{request.request_context.request_id,
                                           RequestReplayStatusDTO::kAcceptedNew,
                                           request.operation_id,
                                           current_revision,
                                           control,
                                           next_revision,
                                           accepted_at_ns};
    accepted_state = active_operation->state;
    accepted_state->operation_revision = next_revision;
    accepted_state->updated_at_ns = accepted_at_ns;
    stamp_operation_capture_locked(*active_operation, *accepted_state);
    active_operation->pending_control = PendingOperationControl{accepted,
                                                                targets.size(),
                                                                0U,
                                                                std::nullopt,
                                                                false,
                                                                false,
                                                                false,
                                                                request.execution_scope};
    control_replay_ledger_.erase_if([&](const ControlReplayEntry& entry) {
      return entry.accepted.operation_id == request.operation_id;
    });
    control_replay_ledger_.push_back(ControlReplayEntry{request, accepted, false});
  }

  auto durable_acceptance = attempt_journal_write([&] {
    return journal_.RecordControlAccepted(config_.request_owner_epoch, request, accepted,
                                          *accepted_state);
  });
  if (!durable_acceptance.ok()) {
    std::lock_guard state_lock(state_mutex_);
    retain_operation_journal_write_fault_locked(
        "control_accepted", durable_acceptance.error(), &*accepted_state,
        request.request_context.request_id);
    const auto replay =
        control_replay_ledger_.find_if(
            accepted.request_id, [](const ControlReplayEntry& entry) { return !entry.durable; });
    if (replay != control_replay_ledger_.end()) {
      control_replay_ledger_.erase(replay);
    }
    auto* active_operation = find_active_operation_locked(request.operation_id);
    if (active_operation != nullptr && active_operation->pending_control.has_value() &&
        active_operation->pending_control->accepted == accepted) {
      active_operation->pending_control.reset();
    }
    return Result<OperationControlAcceptedDTO>::Err(durable_acceptance.error());
  }

  std::optional<StopIntentDTO> reset_stop;
  bool settle_without_effect = false;
  bool defer_effects_to_stop = false;
  {
    std::lock_guard state_lock(state_mutex_);
    const auto replay = control_replay_ledger_.find(accepted.request_id);
    if (replay == control_replay_ledger_.end()) {
      return Result<OperationControlAcceptedDTO>::Ok(accepted);
    }
    replay->durable = true;
    auto* active_operation = find_active_operation_locked(request.operation_id);
    if (active_operation == nullptr || !active_operation->pending_control.has_value() ||
        active_operation->pending_control->accepted != accepted) {
      return Result<OperationControlAcceptedDTO>::Ok(accepted);
    }
    active_operation->pending_control->durable = true;
    const auto terminal_stop_result = active_operation->terminal_stop_result();
    if (active_operation->state.operation_revision != accepted.accepted_against_revision) {
      if (active_operation->state.active_stop_id.has_value()) {
        active_operation->pending_control->superseded_by_stop = true;
        if (control == OperationControlKind::kAcknowledgeCompleted &&
            active_operation->ordinary_pneumatic_acknowledgement()) {
          // A later moving Stop does not consume this ordinary result. Keep
          // its one durable ACK and run the same local effect; settlement
          // still waits for the exact containing certificate and real return.
        } else if (control != OperationControlKind::kReset) {
          active_operation->pending_control.reset();
          defer_effects_to_stop = true;
        } else if (!terminal_stop_result.has_value()) {
          defer_effects_to_stop = true;
        } else {
          // Whatever the Stop's outcome, Reset issues the lane's hard
          // stop if the motion is still winding down and then completes (as
          // `main`); an uncontained Stop no longer holds a cell latch.
          active_operation->pending_control->superseded_by_stop = false;
          settle_without_effect = targets.empty();
        }
      } else {
        active_operation->pending_control.reset();
        defer_effects_to_stop = true;
      }
    } else {
      auto owner_rows =
          retained_owner_rows(active_operation->participants, resource_slots_->coherent_snapshot());
      if (!owner_rows.ok()) {
        active_operation->pending_control.reset();
        return Result<OperationControlAcceptedDTO>::Ok(accepted);
      }
      auto revised =
          owner_rows.value().empty()
              ? Result<std::vector<ResourceSlotOwnerDTO>>::Ok(std::vector<ResourceSlotOwnerDTO>{})
              : resource_slots_->revise_owner_batch(owner_rows.value(),
                                                    accepted.operation_revision_after_acceptance);
      if (!revised.ok()) {
        active_operation->state.state = OperationStateKind::kInterventionRequired;
        active_operation->state.updated_at_ns = clock_.now();
        active_operation->pending_control.reset();
        return Result<OperationControlAcceptedDTO>::Ok(accepted);
      }
      for (const auto& revised_owner : revised.value()) {
        const auto participant = std::find_if(
            active_operation->participants.begin(), active_operation->participants.end(),
            [&](const auto& candidate) {
              return candidate.slot_owner.participant_id == revised_owner.participant_id;
            });
        if (participant == active_operation->participants.end()) {
          continue;
        }
        participant->slot_owner = revised_owner;
      }
      active_operation->state = *accepted_state;
      if (active_operation->runtime_scale_status.has_value()) {
        active_operation->runtime_scale_status->operation_revision =
            accepted.operation_revision_after_acceptance;
        active_operation->runtime_scale_status->updated_at_ns = accepted.accepted_at_ns;
      }
      if (control == OperationControlKind::kReset) {
        if (reset_already_terminal || terminal_stop_result.has_value()) {
          // Any terminal Stop outcome is reset the same way (as `main`).
          active_operation->pending_control->superseded_by_stop = false;
          settle_without_effect = targets.empty();
        } else if (active_operation->state.active_stop_id.has_value()) {
          active_operation->pending_control->superseded_by_stop = true;
          defer_effects_to_stop = true;
        } else {
          reset_stop =
              internal_control_stop_intent_locked(*active_operation, accepted, "operation_reset");
        }
      } else if (control == OperationControlKind::kAcknowledgeCompleted && targets.empty()) {
        settle_without_effect = true;
      }
    }
    if (!defer_effects_to_stop && !reset_stop.has_value() && !settle_without_effect) {
      active_operation->pending_control->effects_dispatched = true;
    }
  }

  commit.release();
  if (defer_effects_to_stop) {
    return Result<OperationControlAcceptedDTO>::Ok(accepted);
  }

  if (settle_without_effect) {
    std::lock_guard state_lock(state_mutex_);
    (void)settle_control_operation_locked(request.operation_id, control);
    return Result<OperationControlAcceptedDTO>::Ok(accepted);
  }
  if (reset_stop.has_value()) {
    auto stopped = accept_stop(EmergencyStopProjectedIntentDTO{std::move(*reset_stop)});
    if (!stopped.ok()) {
      std::lock_guard state_lock(state_mutex_);
      auto* operation = find_active_operation_locked(request.operation_id);
      if (operation != nullptr) {
        operation->state.state = OperationStateKind::kInterventionRequired;
        operation->state.updated_at_ns = clock_.now();
        operation->pending_control.reset();
      }
    }
    return Result<OperationControlAcceptedDTO>::Ok(accepted);
  }

  for (const auto& target : targets) {
    auto started = operation_control_executor_->TryStart(OperationControlEffectRequest{
        target.engine, request.operation_id, target.participant_id,
        accepted.operation_revision_after_acceptance, control, target.subordinate_id,
        target.payload_kind, target.command_target, target.subordinate_generation});
    if (started.status == OperationControlEffectStartStatus::kStarted) {
      continue;
    }
    CompleteOperationControlEffect(OperationControlEffectResult{
        request.operation_id, target.participant_id, accepted.operation_revision_after_acceptance,
        control, false,
        error(std::string(to_string(ErrorCode::kInvariantViolation)),
              "prequalified operation-control lane rejected accepted work",
              "operation_control_effect_handoff_failed")});
  }
  return Result<OperationControlAcceptedDTO>::Ok(accepted);
}

StopIntentDTO ResourceAddressedExecutionKernel::internal_control_stop_intent_locked(
    const ActiveOperation& operation, const OperationControlAcceptedDTO& accepted,
    const std::string_view reason) const {
  const auto material = config_.request_owner_epoch.value() + "\n" + accepted.request_id.value() +
                        "\n" + std::string(control_name(accepted.control)) + "\n" +
                        operation.state.operation_id.value();
  const RequestId request_id(manyforge::content_identity::sha256_digest(material));
  return StopIntentDTO{
      PublicRequestContextDTO{request_id, config_.request_owner_epoch, "kernel.operation_control",
                              accepted.request_id.value()},
      std::vector<StopOperationTargetDTO>{StopOperationTargetDTO{
          operation.state.operation_id, operation.state.operation_revision,
          operation.state.active_stop_generation.value_or(StopGeneration(0U))}},
      std::string(reason),
      RequestAttributionDTO{RequestTransportDTO::kInternal, RequestAssuranceDTO::kUnverified,
                            std::nullopt}};
}

void ResourceAddressedExecutionKernel::settle_completed_control_locked(
    const OperationId& operation_id) {
  const auto* operation = find_active_operation_locked(operation_id);
  if (operation == nullptr || !operation->pending_control.has_value()) return;
  const auto& pending = *operation->pending_control;
  if (pending.durable && pending.effects_dispatched &&
      pending.completed_effects == pending.expected_effects && !pending.first_failure.has_value() &&
      (pending.accepted.control == OperationControlKind::kReset ||
       pending.accepted.control == OperationControlKind::kAcknowledgeCompleted)) {
    (void)settle_control_operation_locked(operation_id, pending.accepted.control);
  }
}

void ResourceAddressedExecutionKernel::wake_stop_after_preparation_borrow_locked(
    const OperationId& operation_id) noexcept {
  // A Stop pass that observed a Prepare call or prepared read still in flight
  // armed only its outer deadline and waits for that borrow to end. Each
  // borrow release wakes the owning Stop in the same lock hold, so a wakeup
  // published while the borrow was still held cannot be its last one.
  const auto* operation = find_active_operation_locked(operation_id);
  if (operation == nullptr || !operation->state.active_stop_id.has_value() ||
      !operation->state.active_stop_generation.has_value()) {
    return;
  }
  for (const auto& transaction : stop_transactions_) {
    if (transaction != nullptr && transaction->active && !transaction->terminal &&
        transaction->stop_id == operation->state.active_stop_id &&
        transaction->stop_generation == *operation->state.active_stop_generation) {
      stop_control_executor_->ArmDeadline(transaction->token, clock_.now());
      return;
    }
  }
}

OperationStateDTO ResourceAddressedExecutionKernel::capture_control_failure_locked(
    ActiveOperation& operation) {
  operation.state.state = OperationStateKind::kInterventionRequired;
  operation.state.updated_at_ns = clock_.now();
  for (const auto& participant : operation.participants) {
    if (!participant.last_subordinate_error.has_value()) continue;
    for (auto& row : operation.state.participants) {
      if (row.participant_id == participant.slot_owner.participant_id) {
        row.state = OperationStateKind::kInterventionRequired;
        row.terminal_error = participant.last_subordinate_error;
        break;
      }
    }
  }
  stamp_operation_capture_locked(operation, operation.state);
  return operation.state;
}

void ResourceAddressedExecutionKernel::clear_motion_continuation_locked(
    const ActiveOperation& operation) {
  if (!program_cycle_claim_run_ ||
      program_cycle_claim_run_->program_run_id != operation.state.program_run_id)
    return;
  for (const auto& participant : operation.participants) {
    if (!participant.continuation || participant.continuation_node_index >=
                                         program_cycle_claim_run_->motion_continuations.size())
      continue;
    auto& slot =
        program_cycle_claim_run_->motion_continuations[participant.continuation_node_index];
    if (slot && slot->intent == participant.continuation) slot.reset();
  }
}

Result<void> ResourceAddressedExecutionKernel::settle_control_operation_locked(
    const OperationId& operation_id, const OperationControlKind control) {
  auto* operation = find_active_operation_locked(operation_id);
  if (operation == nullptr || !operation->pending_control.has_value() ||
      operation->pending_control->accepted.control != control ||
      !operation->pending_control->durable) {
    return reject<void>("operation control settlement has no durable resident claim",
                        "operation_control_settlement_missing", ErrorCode::kInvariantViolation);
  }
  if (operation->has_pending_io_rule_response() ||
      operation->takeover_advance_in_flight || !operation->pending_motion_takeover.empty() ||
      !operation->next_motion_takeover.empty() || operation->ordinary_commit_in_flight ||
      operation->coordinated_cancellation_in_flight ||
      operation->dispatch_effect_in_flight() || operation->reconciliation_in_flight() ||
      std::any_of(operation->participants.begin(), operation->participants.end(),
                  [](const auto& participant) {
                    return participant.preparation_borrow_in_flight();
                  })) {
    return Result<void>::Ok();
  }
  if (control == OperationControlKind::kAcknowledgeCompleted &&
      operation->state.state != OperationStateKind::kCompleted &&
      !operation->pneumatic_final_failure()) {
    return reject<void>("completion acknowledgement lost its terminal state",
                        "operation_acknowledgement_state_changed",
                        ErrorCode::kInvalidStateTransition);
  }
  if (control != OperationControlKind::kAcknowledgeCompleted &&
      control != OperationControlKind::kReset) {
    return reject<void>("nonterminal operation control cannot settle an operation",
                        "operation_control_settlement_kind_invalid", ErrorCode::kInvalidRequest);
  }
  if (operation->state.active_stop_id.has_value()) {
    const auto terminal_stop = operation->terminal_stop_result();
    if (!terminal_stop.has_value()) {
      // Terminal provider feedback alone cannot release Stop-owned resources.
      return Result<void>::Ok();
    }
    const auto outcome = *terminal_stop;
    // A Reset completes after any terminal Stop outcome (its effect has
    // hard-stopped and retired the execution, as `main`).
    if (control != OperationControlKind::kReset &&
        outcome != StopCertificateResultDTO::kSucceededNominal &&
        outcome != StopCertificateResultDTO::kSucceededEscalated) {
      // Consume the exact cleanup result, but never use it to release a
      // resource whose newer Stop still requires containment/reconciliation.
      operation->state.state = OperationStateKind::kInterventionRequired;
      operation->pending_control.reset();
      return Result<void>::Ok();
    }
  }
  return retire_operation_ownership_locked(operation_id, true);
}

Result<void> ResourceAddressedExecutionKernel::retire_operation_ownership_locked(
    const OperationId& operation_id, const bool clear_continuation,
    const bool discard_node_continuations) {
  auto* operation = find_active_operation_locked(operation_id);
  if (!operation)
    return reject<void>("operation retirement has no resident owner", "operation_not_resident",
                        ErrorCode::kMissingReference);
  if (operation->has_pending_io_rule_response() ||
      operation->takeover_advance_in_flight || !operation->pending_motion_takeover.empty() ||
      !operation->next_motion_takeover.empty())
    return reject<void>("accepted motion takeover ownership has not drained",
                        "operation_effect_in_flight", ErrorCode::kInvalidStateTransition);
  const auto rules_retired = retire_io_rules_locked(*operation);
  if (!rules_retired.ok()) return rules_retired;
  auto owner_rows =
      retained_owner_rows(operation->participants, resource_slots_->coherent_snapshot());
  if (!owner_rows.ok()) {
    return Result<void>::Err(owner_rows.error());
  }
  if (!owner_rows.value().empty()) {
    auto released = resource_slots_->release_batch(owner_rows.value());
    if (!released.ok()) {
      return released;
    }
  }
  operation->pending_control.reset();
  if (clear_continuation) clear_motion_continuation_locked(*operation);
  if (discard_node_continuations && program_cycle_claim_run_ && program_cycle_claim_run_->capture &&
      program_cycle_claim_run_->program_run_id == operation->state.program_run_id) {
    const auto& table = *program_cycle_claim_run_->capture->table;
    for (const auto& participant : operation->state.participants) {
      if (!participant.execution_scope) continue;
      const auto node = std::lower_bound(
          table.nodes.begin(), table.nodes.end(), participant.execution_scope->node_instance_id,
          [](const auto& row, const auto& id) { return row.instance_id < id; });
      if (node == table.nodes.end() ||
          node->instance_id != participant.execution_scope->node_instance_id)
        continue;
      const auto index = static_cast<std::size_t>(node - table.nodes.begin());
      if (index < program_cycle_claim_run_->motion_continuations.size()) {
        auto& slot = program_cycle_claim_run_->motion_continuations[index];
        if (slot &&
            (slot->operation_id == operation_id || slot->origin == *participant.execution_scope))
          slot.reset();
      }
    }
  }
  retain_historical_operation_locked(operation->state);
  active_operations_.erase(operation_id);
  return Result<void>::Ok();
}

void ResourceAddressedExecutionKernel::OperationControlEffectCompleted(
    BoundedOperationControlExecutor& executor, const OperationControlEffectHandle handle) noexcept {
  try {
    auto taken = executor.TryTake(handle);
    if (taken.status == OperationControlEffectTakeStatus::kCompleted ||
        taken.status == OperationControlEffectTakeStatus::kInfrastructureFailure) {
      // The moved result no longer needs its bounded worker slot. Make that
      // lane reusable before publishing free resources or retry eligibility.
      if (executor.Release(handle) != OperationControlEffectReleaseStatus::kReleased ||
          !taken.result.has_value()) {
        return;
      }
      CompleteOperationControlEffect(std::move(*taken.result));
    }
  } catch (...) {
  }
}

OperationStateDTO ResourceAddressedExecutionKernel::publish_settlement_fault_locked(
    ActiveOperation& operation) {
  // A cleanup fault is a successor current state, not a replacement of the
  // immutable physical Stop outcome. Reuse the existing operation revision
  // so delayed durable projections have a causal order even at equal clocks.
  auto owners = retained_owner_rows(operation.participants, resource_slots_->coherent_snapshot());
  if (owners.ok() && operation.state.operation_revision.value() < kMaximumKernelJsonInteger) {
    const OperationRevision next(operation.state.operation_revision.value() + 1U);
    auto revised = owners.value().empty()
                       ? Result<std::vector<ResourceSlotOwnerDTO>>::Ok({})
                       : resource_slots_->revise_owner_batch(owners.value(), next);
    if (revised.ok()) {
      for (const auto& owner : revised.value()) {
        const auto participant = std::find_if(
            operation.participants.begin(), operation.participants.end(),
            [&](const auto& row) { return row.slot_owner.participant_id == owner.participant_id; });
        if (participant != operation.participants.end()) participant->slot_owner = owner;
      }
      operation.state.operation_revision = next;
      if (operation.runtime_scale_status.has_value())
        operation.runtime_scale_status->operation_revision = next;
    }
  }
  operation.state.state = OperationStateKind::kInterventionRequired;
  operation.state.updated_at_ns = clock_.now();
  operation.pending_control.reset();
  stamp_operation_capture_locked(operation, operation.state);
  return operation.state;
}

void ResourceAddressedExecutionKernel::CompleteOperationControlEffect(
    OperationControlEffectResult result) noexcept {
  std::optional<OperationStateDTO> settlement_failure;
  try {
    [&] {
      std::lock_guard state_lock(state_mutex_);
      auto* operation = find_active_operation_locked(result.operation_id);
      if (operation == nullptr || !operation->pending_control.has_value() ||
          operation->pending_control->accepted.control != result.control ||
          operation->pending_control->accepted.operation_revision_after_acceptance !=
              result.operation_revision) {
        return;
      }
      auto& pending = *operation->pending_control;
      const auto participant = std::find_if(
          operation->participants.begin(), operation->participants.end(), [&](const auto& value) {
            return value.slot_owner.participant_id == result.participant_id;
          });
      if (participant == operation->participants.end() ||
          pending.completed_effects >= pending.expected_effects) {
        return;
      }
      if (!result.accepted) {
        const auto failure = result.error.value_or(
            error(std::string(to_string(ErrorCode::kExecutionRejected)),
                  std::string("subordinate rejected accepted ") + control_name(result.control),
                  "subordinate_control_rejected"));
        participant->last_subordinate_error = failure;
        const auto public_participant = std::find_if(
            operation->state.participants.begin(), operation->state.participants.end(),
            [&](const auto& row) { return row.participant_id == result.participant_id; });
        if (public_participant != operation->state.participants.end()) {
          public_participant->terminal_error = failure;
          public_participant->state = OperationStateKind::kInterventionRequired;
          stamp_operation_capture_locked(*operation, operation->state);
        }
        if (!pending.first_failure.has_value()) {
          pending.first_failure = failure;
        }
      }
      ++pending.completed_effects;
      const bool ordinary_ack = result.control == OperationControlKind::kAcknowledgeCompleted &&
                                operation->ordinary_pneumatic_acknowledgement();
      const bool awaiting_stop = operation->state.active_stop_id.has_value() &&
                                 !operation->terminal_stop_result().has_value();
      const bool superseded_by_stop =
          (pending.superseded_by_stop || operation->state.active_stop_id.has_value()) &&
          (!ordinary_ack || awaiting_stop);
      if (superseded_by_stop && result.control != OperationControlKind::kReset) {
        // Retain the exact control failure until the Stop terminal handoff.
        // No provider call runs under the state lock.
        if (operation->terminal_stop_result().has_value() &&
            pending.completed_effects == pending.expected_effects) {
          operation->pending_control.reset();
        }
      } else if (pending.completed_effects != pending.expected_effects) {
        // Terminal Reset/ACK cleanup waits for every effect.
        return;
      } else if (result.control == OperationControlKind::kReset) {
        if (pending.first_failure.has_value()) {
          if (operation->state.active_stop_id.has_value() &&
              !operation->terminal_stop_result().has_value()) {
            // The exact Stop certificate will publish this completed fault;
            // do not race its same-revision intermediate state with a write.
            return;
          }
          // Persist the failure in the existing owner state. A clean terminal
          // state would let an automatic owner repeatedly retry settlement.
          settlement_failure = publish_settlement_fault_locked(*operation);
        } else {
          (void)settle_control_operation_locked(result.operation_id, result.control);
        }
        return;
      } else if (!pending.first_failure.has_value()) {
        (void)settle_control_operation_locked(result.operation_id, result.control);
        return;
      } else if (!superseded_by_stop) {
        // This known operation retains its exact resource reservation until
        // explicit owner reconciliation. Do not turn a scoped settlement
        // failure into the permanent cell-wide invariant-failure latch.
        settlement_failure = publish_settlement_fault_locked(*operation);
      }
    }();
    if (settlement_failure.has_value()) {
      // Fault visibility and scoped fencing must not wait behind ordinary
      // storage, and this write must never hold the emergency capture lock.
      const auto persisted = [&]() -> Result<std::uint64_t> {
        try {
          return journal_.RecordOperationState(
              config_.request_owner_epoch, *settlement_failure, nullptr,
              result.control == OperationControlKind::kReset
                  ? "operation_reset_failed"
                  : "operation_acknowledge_completed_failed");
        } catch (...) {
          return Result<std::uint64_t>::Err(error("operation_settlement_fault_persistence_threw",
                                                  "settlement fault persistence threw",
                                                  "settlement_fault_persistence_failed"));
        }
      }();
      if (!persisted.ok()) {
        std::lock_guard state_lock(state_mutex_);
        retain_operation_journal_write_fault_locked(
            result.control == OperationControlKind::kReset
                ? "operation_reset_failed" : "operation_acknowledge_completed_failed",
            persisted.error(), &*settlement_failure);
        auto* operation = find_active_operation_locked(result.operation_id);
        if (operation != nullptr && operation->state == *settlement_failure) {
          for (auto& row : operation->state.participants) {
            if (row.terminal_error.has_value()) {
              row.terminal_error->details["settlement_fault_persistence"] = "failed";
              row.terminal_error->details["persistence_error_code"] = persisted.error().code;
            }
          }
        }
      }
    }
  } catch (...) {
  }
}

Result<StopAcceptedDTO> ResourceAddressedExecutionKernel::stop(const StopIntentDTO& intent) {
  return accept_stop(EmergencyStopProjectedIntentDTO{intent});
}

Result<StopAcceptedDTO> ResourceAddressedExecutionKernel::full_stop(
    const FullStopIntentDTO& intent) {
  return accept_stop(EmergencyStopProjectedIntentDTO{intent});
}

Result<std::optional<ResolvedManualProgramHandover>>
ResourceAddressedExecutionKernel::get_manual_program_handover(const OperationIntentDTO& intent) {
  // Stabilize the physical registration inventory without taking Stop's lock
  // across resolution, allocation or the potentially large captured table.
  std::lock_guard submission_lock(submission_mutex_);
  return resolve_manual_program_handover(intent);
}

Result<ResolvedManualControlAvailability>
ResourceAddressedExecutionKernel::get_manual_control_availability(
    const OperationIntentDTO& intent) {
  using Reply = Result<ResolvedManualControlAvailability>;
  std::lock_guard submission_lock(submission_mutex_);
  const auto valid = validate_operation_intent(intent);
  if (!valid.ok()) return Reply::Err(valid.error());
  if (intent.request_context.request_owner_epoch != config_.request_owner_epoch ||
      intent.participants.size() != 1U ||
      intent.coordination_policy != OperationCoordinationPolicy::kIndependent ||
      intent.participants.front().execution_scope)
    return reject<ResolvedManualControlAvailability>(
        "manual availability requires a current direct target", "manual_handover_intent_invalid");
  const auto found = resolve_registration(intent, 0U);
  if (!found.ok()) return Reply::Err(found.error());
  if (!found.value().intrinsic_direct_control)
    return reject<ResolvedManualControlAvailability>(
        "manual availability requires an intrinsic direct target",
        "direct_control_product_mismatch");
  ResolvedManualControlAvailability result;
  const auto unavailable = [&](const ErrorDTO& error) {
    const auto reason = error.details.find("reason");
    result.unavailable_reason = reason == error.details.end() ? error.code : reason->second;
    result.cleanup_required = false;
    return Reply::Ok(std::move(result));
  };
  auto resolved = resolve_manual_program_handover(found.value());
  if (!resolved.ok()) return unavailable(resolved.error());
  result.handover = std::move(resolved.value());
  auto& handover = result.handover;
  std::optional<CellAdmissionClaimGeneration> parent;
  if (handover) parent = handover->capture.claim_generation;
  // Keep the same admission -> state ordering as actual claim acquisition.
  // Stop bypasses this ordinary read just as it bypasses the claim write.
  std::lock_guard admission_lock(*admission_mutex_);
  const auto ready = cell_admission_owner_->CheckManualCellAdmissionReadiness(parent);
  if (!ready.ok()) return unavailable(ready.error());
  const auto& binding = found.value().binding.value();
  if (!binding.control_path || !found.value().stop_readiness)
    return unavailable(
        {"manual_control_not_ready", "target has no current route/Stop readiness", {}});
  const auto& control = *binding.control_path;
  const auto route = runtime_route_configuration_readbacks_.find(control.control_domain_id);
  if (runtime_route_configuration_publication_required_.contains(control.control_domain_id) ||
      (route != runtime_route_configuration_readbacks_.end() &&
       (!route->second.operation_admission_open ||
        route->second.active_configuration.active_control_configuration_id !=
            control.active_control_configuration_id ||
        route->second.active_configuration.active_control_configuration_generation !=
            control.active_control_configuration_generation)))
    return unavailable(
        {"runtime_route_configuration_not_ready", "target route is not current", {}});
  if (auto refusal = component_command_refusal_locked(found.value()); refusal.has_value())
    return unavailable(*refusal);
  std::vector<ResourceId> resources;
  resources.reserve(binding.resources.size());
  for (const auto& resource : binding.resources) resources.push_back(resource.resource_id);
  std::sort(resources.begin(), resources.end());
  std::lock_guard state_lock(state_mutex_);
  if (motion_admission_closed_locked() || motion_admission_unreconciled_)
    return unavailable({"motion_admission_closed", "motion admission is closed", {}});
  if (handover) {
    const auto held = validate_manual_program_handover_locked(*handover);
    if (!held.ok()) return unavailable(held.error());
  } else if (program_cycle_claim_run_) {
    return unavailable({"manual_handover_scope_stale", "a Program acquired the target", {}});
  }
  for (const auto& [id, operation] : active_operations_) {
    (void)id;
    const bool overlaps =
        std::any_of(operation->participants.begin(), operation->participants.end(),
                    [&](const auto& participant) {
                      return participant.engine == found.value().engine ||
                             std::any_of(participant.resources.begin(), participant.resources.end(),
                                         [&](const auto& resource) {
                                           return std::binary_search(resources.begin(),
                                                                     resources.end(), resource);
                                         });
                    });
    if (!overlaps) continue;
    const bool settled =
        operation->terminal_stop_result() == StopCertificateResultDTO::kSucceededNominal ||
        operation->state.state == OperationStateKind::kCompleted ||
        operation->state.state == OperationStateKind::kCancelledPreDispatch;
    if (!handover || operation->intent.program_run_id != handover->capture.program_run_id ||
        !settled || operation->dispatch_effect_in_flight() ||
        operation->recovery_effect_in_flight || operation->pending_control ||
        operation->stop_effect_in_flight)
      return unavailable({"manual_control_target_busy", "target operation is not settled", {}});
    result.cleanup_required = true;
  }
  result.available = true;
  return Reply::Ok(std::move(result));
}

Result<std::optional<ResolvedManualProgramHandover>>
ResourceAddressedExecutionKernel::resolve_manual_program_handover(
    const OperationIntentDTO& intent) {
  using Reply = Result<std::optional<ResolvedManualProgramHandover>>;
  const auto valid = validate_operation_intent(intent);
  if (!valid.ok()) return Reply::Err(valid.error());
  if (intent.request_context.request_owner_epoch != config_.request_owner_epoch ||
      intent.participants.size() != 1U ||
      intent.coordination_policy != OperationCoordinationPolicy::kIndependent ||
      intent.participants.front().execution_scope) {
    return reject<std::optional<ResolvedManualProgramHandover>>(
        "manual handover requires one current direct-control target",
        "manual_handover_intent_invalid");
  }
  auto manual = resolve_registration(intent, 0U);
  if (!manual.ok()) return Reply::Err(manual.error());
  if (!manual.value().intrinsic_direct_control) {
    return reject<std::optional<ResolvedManualProgramHandover>>(
        "manual handover requires an intrinsic direct-control binding",
        "direct_control_product_mismatch");
  }
  return resolve_manual_program_handover(manual.value());
}

Result<std::optional<ResolvedManualProgramHandover>>
ResourceAddressedExecutionKernel::resolve_manual_program_handover(
    const RuntimeParticipantExecutionRegistration& registration) {
  using Reply = Result<std::optional<ResolvedManualProgramHandover>>;
  std::optional<ResolvedManualProgramHandover> captured;
  {
    std::lock_guard lock(state_mutex_);
    if (motion_admission_closed_locked() || motion_admission_unreconciled_) {
      return reject<std::optional<ResolvedManualProgramHandover>>(
          "motion admission is closed or unreconciled", "motion_admission_closed");
    }
    if (!program_cycle_claim_run_) return Reply::Ok(std::nullopt);
    const auto& parent = *program_cycle_claim_run_;
    const auto fence = program_run_stop_fences_.find(parent.program_run_id);
    if (!parent.capture || !parent.capture->binding || fence == program_run_stop_fences_.end()) {
      return reject<std::optional<ResolvedManualProgramHandover>>(
          "Program target mapping or hold is unavailable", "manual_handover_capture_unavailable");
    }
    captured.emplace(ResolvedManualProgramHandover{*parent.capture, fence->selection_revision, {}});
  }
  auto& result = *captured;

  std::map<ProgramTargetReferenceDTO, bool> conflicts;
  const auto& targets = result.capture.binding->target_bindings;
  const auto& nodes = result.capture.table->nodes;
  const auto& manual_resources = registration.binding.value().resources;
  for (std::size_t index = 0U; index < nodes.size(); ++index) {
    bool selected = false;
    for (const auto& effect : nodes[index].target_effects) {
      if (!effect.effectful) continue;
      auto cached = conflicts.find(effect.target);
      if (cached == conflicts.end()) {
        const auto target = std::lower_bound(
            targets.begin(), targets.end(), effect.target,
            [](const auto& row, const auto& id) { return row.target < id; });
        if (target == targets.end() || target->target != effect.target) {
          return reject<std::optional<ResolvedManualProgramHandover>>(
              "captured producer has no exact Program target", "manual_handover_target_unresolved");
        }
        bool found = false;
        bool overlap = false;
        for (const auto& registration : registrations_) {
          const auto& binding = registration.binding.value();
          if (binding.controllable_component_id != target->target.component_id ||
              binding.command_target_id != target->target.command_target_id)
            continue;
          found = true;
          overlap =
              overlap ||
              std::any_of(
                  binding.resources.begin(), binding.resources.end(), [&](const auto& resource) {
                    return std::any_of(manual_resources.begin(), manual_resources.end(),
                                       [&](const auto& manual_resource) {
                                         return resource.resource_id == manual_resource.resource_id;
                                       });
                  });
        }
        if (!found) {
          return reject<std::optional<ResolvedManualProgramHandover>>(
              "captured producer has no current physical target",
              "manual_handover_target_unresolved");
        }
        cached = conflicts.emplace(effect.target, overlap).first;
      }
      selected = selected || cached->second;
    }
    if (selected) result.producer_node_indices.push_back(static_cast<std::uint32_t>(index));
  }

  {
    std::lock_guard lock(state_mutex_);
    const auto valid_hold = validate_manual_program_handover_locked(result);
    if (!valid_hold.ok()) return Reply::Err(valid_hold.error());
  }
  return Reply::Ok(std::move(result));
}

Result<void> ResourceAddressedExecutionKernel::validate_manual_program_handover_locked(
    const ResolvedManualProgramHandover& result) const {
  const auto fence = program_run_stop_fences_.find(result.capture.program_run_id);
  if (motion_admission_closed_locked() || motion_admission_unreconciled_ ||
      !program_cycle_claim_run_ ||
      program_cycle_claim_run_->claim_generation != result.capture.claim_generation ||
      !program_cycle_claim_run_->capture ||
      program_cycle_claim_run_->capture->table != result.capture.table ||
      program_cycle_claim_run_->capture->binding != result.capture.binding ||
      fence == program_run_stop_fences_.end() ||
      fence->selection_revision != result.selection_revision ||
      (fence->stop_id && !fence->releasable)) {
    return reject<void>("Program hold changed or is not settled", "manual_handover_scope_stale");
  }
  for (const auto index : result.producer_node_indices) {
    // Ready producers never started, so they are as quiet as Held ones.
    if (index >= fence->node_states.size() ||
        (fence->node_states[index] != ProgramNodeHoldStateDTO::kHeld &&
         fence->node_states[index] != ProgramNodeHoldStateDTO::kReady)) {
      return reject<void>("a conflicting Program producer is running, stopping or faulted",
                          "manual_handover_producer_not_held");
    }
  }
  if (std::any_of(
          fence->pending_effects.begin(), fence->pending_effects.end(), [&](const auto& effect) {
            return std::binary_search(result.producer_node_indices.begin(),
                                      result.producer_node_indices.end(), effect.node_index);
          })) {
    return reject<void>("selected Stop effects have not settled", "manual_handover_stop_unsettled");
  }
  return Result<void>::Ok();
}

Result<void> ResourceAddressedExecutionKernel::register_program_run_capture(
    ProgramRunExecutionCaptureDTO capture) {
  const auto bounded = [](const auto& id) {
    return !id.empty() && id.value().size() <= 128U && id.value().find('\0') == std::string::npos;
  };
  if (!bounded(capture.request_owner_epoch) || !bounded(capture.program_runner_owner_epoch) ||
      !bounded(capture.program_run_id) || !bounded(capture.program_id) ||
      !bounded(capture.program_revision) || !bounded(capture.program_binding_id) ||
      !bounded(capture.retained_bundle_id) || !bounded(capture.graph_revision) ||
      !capture.claim_generation.nonzero() || capture.table == nullptr || !capture.binding ||
      capture.table->nodes.empty() || capture.table->nodes.size() > kMaxProgramTreeNodes ||
      capture.table->named_selections.size() > kMaxProgramExecutionGroups) {
    return reject<void>("Program execution capture is incomplete or exceeds its finite bound",
                        "program_execution_capture_invalid", ErrorCode::kInvalidRequest);
  }
  if (capture.binding->program_binding_id != capture.program_binding_id ||
      capture.binding->program_id != capture.program_id ||
      capture.binding->program_revision != capture.program_revision ||
      capture.binding->retained_bundle_id != capture.retained_bundle_id ||
      capture.binding->graph_revision != capture.graph_revision) {
    return reject<void>("Program capture Binding differs from its exact product",
                        "program_execution_capture_binding_mismatch", ErrorCode::kInvalidRequest);
  }
  // This validates a trusted composition input once, outside Stop/admission
  // ordering. Artifact provenance remains the sealed Program/Binding owner's
  // responsibility; a public client never supplies this table.
  std::size_t effect_count = 0U;
  for (std::size_t index = 0U; index < capture.table->nodes.size(); ++index) {
    const auto& node = capture.table->nodes[index];
    if (!bounded(node.instance_id) || node.owner_api.empty() || node.owner_api.size() > 128U ||
        node.preemption_priority > 100U ||
        (index > 0U && !(capture.table->nodes[index - 1U].instance_id < node.instance_id)) ||
        node.target_effects.size() > kMaxProgramTargetEffects - effect_count) {
      return reject<void>("Program execution nodes are not canonical and bounded",
                          "program_execution_capture_nodes_invalid", ErrorCode::kInvalidRequest);
    }
    if ((node.effective_operator_stop_policy != ProgramOperatorStopPolicy::kHold &&
         node.effective_operator_stop_policy != ProgramOperatorStopPolicy::kContinue) ||
        (node.parent_node_index && *node.parent_node_index >= capture.table->nodes.size()))
      return reject<void>("Program Stop policy/topology is invalid",
                          "program_execution_capture_nodes_invalid", ErrorCode::kInvalidRequest);
    effect_count += node.target_effects.size();
    for (std::size_t effect = 0U; effect < node.target_effects.size(); ++effect) {
      const auto& value = node.target_effects[effect];
      if (!bounded(value.target.component_id) || !bounded(value.target.command_target_id) || !bounded(value.capability_id) ||
          (effect > 0U && !(std::tie(node.target_effects[effect - 1U].target,
                                     node.target_effects[effect - 1U].capability_id) <
                            std::tie(value.target, value.capability_id)))) {
        return reject<void>("Program execution effects are not canonical",
                            "program_execution_capture_effects_invalid",
                            ErrorCode::kInvalidRequest);
      }
    }
  }
  std::vector<std::size_t> child_counts(capture.table->nodes.size(), 0U);
  std::vector<std::uint32_t> leaves;
  leaves.reserve(capture.table->nodes.size());
  for (const auto& node : capture.table->nodes)
    if (node.parent_node_index) ++child_counts[*node.parent_node_index];
  for (std::size_t index = 0U; index < child_counts.size(); ++index)
    if (child_counts[index] == 0U) leaves.push_back(static_cast<std::uint32_t>(index));
  for (std::size_t cursor = 0U; cursor < leaves.size(); ++cursor)
    if (const auto parent = capture.table->nodes[leaves[cursor]].parent_node_index)
      if (--child_counts[*parent] == 0U) leaves.push_back(*parent);
  if (leaves.size() != capture.table->nodes.size())
    return reject<void>("Program parent topology is cyclic",
                        "program_execution_capture_nodes_invalid", ErrorCode::kInvalidRequest);
  std::size_t membership_count = 0U;
  std::set<std::uint32_t> synchronized_nodes;
  for (std::size_t index = 0U; index < capture.table->named_selections.size(); ++index) {
    const auto& selection = capture.table->named_selections[index];
    if (!valid_program_execution_group(selection.group_id) || selection.group_id == "all" ||
        (index > 0U &&
         capture.table->named_selections[index - 1U].group_id >= selection.group_id) ||
        selection.node_indices.empty() ||
        selection.node_indices.size() > kMaxProgramExecutionMemberships - membership_count ||
        !std::is_sorted(selection.node_indices.begin(), selection.node_indices.end()) ||
        std::adjacent_find(selection.node_indices.begin(), selection.node_indices.end()) !=
            selection.node_indices.end() ||
        selection.node_indices.back() >= capture.table->nodes.size()) {
      return reject<void>("Program execution selections are not canonical and bounded",
                          "program_execution_capture_selections_invalid",
                          ErrorCode::kInvalidRequest);
    }
    membership_count += selection.node_indices.size();
    if (selection.execution_policy != ProgramExecutionPolicy::kIndependent) {
      if (selection.node_indices.size() < 2U) {
        return reject<void>("coordinated Program selection is too small",
                            "program_execution_capture_synchronization_invalid",
                            ErrorCode::kInvalidRequest);
      }
      for (const auto node_index : selection.node_indices) {
        const auto& node = capture.table->nodes[node_index];
        if (!synchronized_nodes.insert(node_index).second || node.owner_api != "kernel.operation" ||
            !std::any_of(
                node.target_effects.begin(), node.target_effects.end(), [](const auto& effect) {
                  return effect.effectful &&
                         effect.capability_id == CapabilityId("execution.trajectory.execute");
                })) {
          return reject<void>("synchronized selections overlap or contain non-motion",
                              "program_execution_capture_synchronization_invalid",
                              ErrorCode::kInvalidRequest);
        }
      }
    }
  }

  const auto coordinates_current = [&] {
    return capture.request_owner_epoch == config_.request_owner_epoch &&
           capture.graph_revision == stop_graph_revision_ &&
           capture.retained_bundle_id == stop_retained_bundle_id_ &&
           program_cycle_claim_run_.has_value() &&
           program_cycle_claim_run_->claim_generation == capture.claim_generation &&
           program_cycle_claim_run_->program_run_id == capture.program_run_id &&
           program_cycle_claim_run_->program_runner_owner_epoch ==
               capture.program_runner_owner_epoch &&
           program_cycle_claim_run_->program_revision == capture.program_revision;
  };
  std::optional<ProgramRunExecutionCaptureDTO> previous;
  std::vector<ProgramNodeHoldStateDTO> node_states;
  node_states.reserve(capture.table->nodes.size());
  for (const auto& node : capture.table->nodes) {
    const bool effectful = std::any_of(
        node.target_effects.begin(), node.target_effects.end(),
        [](const auto& effect) { return effect.effectful; });
    node_states.push_back(effectful ? ProgramNodeHoldStateDTO::kReady
                                    : ProgramNodeHoldStateDTO::kRunning);
  }
  std::vector<std::shared_ptr<const ProgramCycleClaimRun::MotionContinuation>> continuations(
      capture.table->nodes.size());
  {
    std::lock_guard state_lock(state_mutex_);
    if (!coordinates_current()) {
      return reject<void>(
          "Program execution capture does not match the current owner/claim/product",
          "program_execution_capture_stale", ErrorCode::kInvalidStateTransition);
    }
    auto& current = program_cycle_claim_run_->capture;
    if (!current.has_value()) {
      if (pending_program_run_admission_ == capture.program_run_id ||
          std::any_of(active_operations_.begin(), active_operations_.end(), [&](const auto& item) {
            return item.second->state.program_run_id == capture.program_run_id;
          })) {
        return reject<void>("Program execution capture must precede all effects",
                            "program_execution_capture_late", ErrorCode::kInvalidStateTransition);
      }
      // A Stop overtaking registration still fences every later effect.
      auto fence = program_run_stop_fences_.find(capture.program_run_id);
      if (fence == program_run_stop_fences_.end()) {
        if (program_run_stop_fences_.size() >= config_.request_replay_capacity) {
          return reject<void>("Program execution scope capacity is exhausted",
                              "program_execution_scope_capacity_exhausted",
                              ErrorCode::kInvalidStateTransition);
        }
        program_run_stop_fences_.push_back(
            ProgramRunStopFence{capture.program_run_id, std::nullopt, StopGeneration(0U)});
        fence = std::prev(program_run_stop_fences_.end());
      }
      // A Stop that overtook registration keeps the initial Ready/Running
      // states: nothing has started, and its stop identity already fences
      // every admission until it is released.
      fence->node_states = std::move(node_states);
      program_cycle_claim_run_->motion_continuations = std::move(continuations);
      current = std::move(capture);
      return Result<void>::Ok();
    }
    previous = *current;
  }
  // Large immutable tables are compared and released outside Stop ordering.
  if (previous->program_id != capture.program_id ||
      previous->program_binding_id != capture.program_binding_id ||
      (previous->binding != capture.binding &&
       (!previous->binding || !capture.binding || *previous->binding != *capture.binding)) ||
      (previous->table != capture.table && *previous->table != *capture.table)) {
    return reject<void>("Program-run identity cannot be rebound to a different capture",
                        "program_execution_capture_rebound", ErrorCode::kInvalidStateTransition);
  }
  std::lock_guard state_lock(state_mutex_);
  if (!coordinates_current() || !program_cycle_claim_run_->capture.has_value() ||
      program_cycle_claim_run_->capture->table != previous->table) {
    return reject<void>("Program execution capture does not match the current owner/claim/product",
                        "program_execution_capture_stale", ErrorCode::kInvalidStateTransition);
  }
  return Result<void>::Ok();
}

Result<void> ResourceAddressedExecutionKernel::validate_program_node_claim(
    const ProgramNodeAdmissionEvidenceDTO& evidence) const {
  std::lock_guard state_lock(state_mutex_);
  if (motion_admission_closed_locked() || motion_admission_unreconciled_ ||
      !program_cycle_claim_run_.has_value() || !program_cycle_claim_run_->capture.has_value() ||
      program_cycle_claim_run_->program_run_id != evidence.program_run_id ||
      program_cycle_claim_run_->program_runner_owner_epoch != evidence.program_runner_owner_epoch ||
      program_cycle_claim_run_->claim_generation !=
          evidence.parent_program_cycle_claim_generation) {
    return reject<void>("node transaction has no current Program capture",
                        "program_node_parent_not_current", ErrorCode::kInvalidStateTransition);
  }
  const auto& nodes = program_cycle_claim_run_->capture->table->nodes;
  const auto node = std::lower_bound(
      nodes.begin(), nodes.end(), evidence.node_instance_id,
      [](const auto& row, const auto& id) { return row.instance_id.value() < id; });
  if (node == nodes.end() || node->instance_id.value() != evidence.node_instance_id ||
      node->owner_api != evidence.owner_api) {
    return reject<void>("node transaction does not match its sealed owner API",
                        "program_execution_scope_unknown", ErrorCode::kInvalidRequest);
  }
  const auto fence =
      program_run_stop_fences_.find(evidence.program_run_id);
  const auto index = static_cast<std::size_t>(node - nodes.begin());
  if (fence == program_run_stop_fences_.end() || index >= fence->node_states.size()) {
    return reject<void>("node transaction has no current execution scope",
                        "program_execution_scope_unknown", ErrorCode::kInvalidStateTransition);
  }
  if (!program_node_effectively_running_locked(evidence.program_run_id, index) ||
      ((fence->stop_id.has_value() ||
        fence->node_states[index] != ProgramNodeHoldStateDTO::kRunning) &&
       !program_node_non_motion_locked(evidence.program_run_id, index))) {
    return Result<void>::Err({"invalid_state_transition",
                              "node transaction is held by an accepted Program Stop",
                              {{"reason", "program_node_stop_fence_active"},
                               {"selection_revision", std::to_string(fence->selection_revision)}}});
  }
  return Result<void>::Ok();
}

bool ResourceAddressedExecutionKernel::program_node_effectively_running_locked(
    const ProgramRunId& run, const std::size_t index) const {
  if (!program_cycle_claim_run_ || program_cycle_claim_run_->program_run_id != run ||
      !program_cycle_claim_run_->capture || !program_cycle_claim_run_->capture->table) return false;
  const auto& nodes = program_cycle_claim_run_->capture->table->nodes;
  const auto fence = program_run_stop_fences_.find(run);
  if (fence == program_run_stop_fences_.end() || index >= nodes.size() ||
      fence->node_states.size() != nodes.size()) return false;
  const auto state = fence->node_states[index];
  if (state != ProgramNodeHoldStateDTO::kRunning && state != ProgramNodeHoldStateDTO::kHeld)
    return false;
  // Match Behavior's actual selected held-subtree closure in one bounded
  // bottom-up traversal, independent of canonical ID order and tree depth.
  std::vector<std::size_t> remaining_children(nodes.size(), 0U);
  std::vector<bool> wholly_held(nodes.size(), false);
  std::vector<std::uint32_t> ready;
  ready.reserve(nodes.size());
  for (std::size_t child = 0U; child < nodes.size(); ++child) {
    wholly_held[child] = fence->node_states[child] != ProgramNodeHoldStateDTO::kRunning;
    if (nodes[child].parent_node_index) ++remaining_children[*nodes[child].parent_node_index];
  }
  for (std::size_t child = 0U; child < nodes.size(); ++child)
    if (remaining_children[child] == 0U) ready.push_back(static_cast<std::uint32_t>(child));
  for (std::size_t cursor = 0U; cursor < ready.size(); ++cursor) {
    const auto child = ready[cursor];
    if (const auto parent = nodes[child].parent_node_index) {
      wholly_held[*parent] = wholly_held[*parent] && wholly_held[child];
      if (--remaining_children[*parent] == 0U) ready.push_back(*parent);
    }
  }
  if (ready.size() != nodes.size()) return false;
  auto current = std::optional<std::uint32_t>(static_cast<std::uint32_t>(index));
  while (current) {
    if (nodes[*current].effective_operator_stop_policy == ProgramOperatorStopPolicy::kHold &&
        wholly_held[*current]) return false;
    current = nodes[*current].parent_node_index;
  }
  return true;
}

bool ResourceAddressedExecutionKernel::program_node_non_motion_locked(
    const ProgramRunId& run, const std::size_t index) const {
  if (!program_cycle_claim_run_ || program_cycle_claim_run_->program_run_id != run ||
      !program_cycle_claim_run_->capture || !program_cycle_claim_run_->capture->binding ||
      !program_cycle_claim_run_->capture->table ||
      index >= program_cycle_claim_run_->capture->table->nodes.size()) return false;
  const auto& capture = *program_cycle_claim_run_->capture;
  const auto& node = capture.table->nodes[index];
  if (node.owner_api != "kernel.command_target") return false;
  bool found_effect = false;
  for (const auto& effect : node.target_effects) {
    if (!effect.effectful) continue;
    found_effect = true;
    const auto& targets = capture.binding->target_bindings;
    const auto target = std::find_if(targets.begin(), targets.end(),
        [&](const auto& row) { return row.target == effect.target; });
    if (target == targets.end()) return false;
    const auto registration = std::find_if(registrations_.begin(), registrations_.end(),
        [&](const auto& row) {
          const auto& binding = row.binding.value();
          return binding.controllable_component_id == target->target.component_id &&
                 binding.command_target_id == target->target.command_target_id &&
                 binding.capability_id == effect.capability_id;
        });
    if (registration == registrations_.end() || !non_motion_registration(*registration)) return false;
  }
  return found_effect;
}

void ResourceAddressedExecutionKernel::fence_held_prepared_program_commands_locked(
    const ProgramRunId& run) {
  if (!program_cycle_claim_run_ || program_cycle_claim_run_->program_run_id != run ||
      !program_cycle_claim_run_->capture || !program_cycle_claim_run_->capture->table) return;
  const auto& nodes = program_cycle_claim_run_->capture->table->nodes;
  for (auto& [id, operation] : active_operations_) {
    if (operation->state.program_run_id != run) continue;
    for (auto& participant : operation->participants) {
      if (!participant.non_motion_transaction() || !participant.engine ||
          participant.preparation_id == 0U) continue;
      const auto assignment = std::find_if(operation->accepted.participant_assignments.begin(),
          operation->accepted.participant_assignments.end(), [&](const auto& row) {
            return row.participant_id == participant.slot_owner.participant_id;
          });
      if (assignment == operation->accepted.participant_assignments.end() ||
          assignment->input_index >= operation->intent.participants.size()) continue;
      const auto& intent = operation->intent.participants[assignment->input_index];
      if (!intent.execution_scope) continue;
      const auto node = std::lower_bound(nodes.begin(), nodes.end(), intent.execution_scope->node_instance_id,
          [](const auto& row, const auto& value) { return row.instance_id < value; });
      if (node == nodes.end() || node->instance_id != intent.execution_scope->node_instance_id ||
          program_node_effectively_running_locked(run, static_cast<std::size_t>(node - nodes.begin()))) continue;
      // The existing engine mutex serializes this no-effect Prepared fence
      // against Prepared -> DispatchRequested. Committed delivery remains owed.
      if (participant.engine->fence_addressed_participant_stop(participant.preparation_id) ==
          AddressedParticipantStopFenceDisposition::kEffectPreventedBeforeDispatch)
        participant.stop_cancelled_pre_dispatch = true;
    }
  }
}

Result<void> ResourceAddressedExecutionKernel::validate_program_execution_scope_locked(
    const OperationIntentDTO& intent, const std::optional<bool> non_motion_only,
    const bool intrinsic_direct_control) const {
  const bool run_stopped =
      program_run_stop_fences_.find_if(intent.program_run_id, [](const auto& fence) {
        return fence.stop_id.has_value();
      }) != program_run_stop_fences_.end();
  // The first pass precedes product resolution; the reservation pass repeats
  // this check with independently resolved kind/readiness for every participant.
  if (run_stopped && non_motion_only.has_value() && !*non_motion_only)
    return reject<void>("the Program run is closed by an accepted Stop",
                        "program_run_stop_fence_active", ErrorCode::kInvalidStateTransition);
  const auto* claim = program_cycle_claim_run_.has_value() &&
                              program_cycle_claim_run_->program_run_id == intent.program_run_id
                          ? &*program_cycle_claim_run_
                          : nullptr;
  const bool scoped = std::any_of(intent.participants.begin(), intent.participants.end(),
                                  [](const auto& row) { return row.execution_scope.has_value(); });
  if (!scoped && claim == nullptr) {
    if (run_stopped && non_motion_only.has_value() && !intrinsic_direct_control)
      return reject<void>("Program Stop admission requires an exact captured node or intrinsic manual owner",
                          "program_execution_capture_missing", ErrorCode::kInvalidStateTransition);
    // Intrinsic direct-control admission is still independently verified by
    // the product-backed resolver. A diagnostic label is not an exemption.
    return Result<void>::Ok();
  }
  if (claim == nullptr || !claim->capture.has_value()) {
    return reject<void>("Program execution has no exact registered capture",
                        "program_execution_capture_missing", ErrorCode::kInvalidStateTransition);
  }
  const auto& capture = *claim->capture;
  if (capture.program_id != intent.program_id ||
      capture.program_revision != intent.program_revision ||
      capture.program_binding_id != intent.program_binding_id ||
      capture.graph_revision != intent.expected_graph_revision ||
      capture.retained_bundle_id != intent.expected_retained_bundle_id) {
    return reject<void>("operation differs from its captured Program/Binding",
                        "program_execution_capture_mismatch", ErrorCode::kInvalidRequest);
  }
  std::vector<std::uint32_t> scoped_node_indices;
  scoped_node_indices.reserve(intent.participants.size());
  for (const auto& participant : intent.participants) {
    if (!participant.execution_scope.has_value()) {
      return reject<void>("captured Program effects require their original authored scope",
                          "program_execution_scope_missing", ErrorCode::kInvalidRequest);
    }
    const auto& scope = *participant.execution_scope;
    const auto node = std::lower_bound(
        capture.table->nodes.begin(), capture.table->nodes.end(), scope.node_instance_id,
        [](const auto& row, const auto& id) { return row.instance_id < id; });
    if (node == capture.table->nodes.end() || node->instance_id != scope.node_instance_id ||
        (node->owner_api != "kernel.operation" && node->owner_api != "kernel.command_target")) {
      return reject<void>("operation has no captured effectful node scope",
                          "program_execution_scope_unknown", ErrorCode::kInvalidRequest);
    }
    const auto index = static_cast<std::size_t>(node - capture.table->nodes.begin());
    scoped_node_indices.push_back(static_cast<std::uint32_t>(index));
    const auto fence =
        program_run_stop_fences_.find(intent.program_run_id);
    if (fence == program_run_stop_fences_.end() || index >= fence->node_states.size() ||
        !program_node_effectively_running_locked(intent.program_run_id, index) ||
        (fence->node_states[index] != ProgramNodeHoldStateDTO::kRunning &&
         non_motion_only.has_value() && !*non_motion_only)) {
      return reject<void>("the authored node is held by an accepted Program Stop",
                          "program_node_stop_fence_active", ErrorCode::kInvalidStateTransition);
    }
    const auto key = std::tie(participant.target, participant.required_capability_id);
    const auto effect =
        std::lower_bound(node->target_effects.begin(), node->target_effects.end(), key,
                         [](const auto& row, const auto& value) {
                           return std::tie(row.target, row.capability_id) < value;
                         });
    if (effect == node->target_effects.end() ||
        std::tie(effect->target, effect->capability_id) != key || !effect->effectful) {
      return reject<void>("operation target/capability is not a captured effectful association",
                          "program_execution_scope_effect_mismatch", ErrorCode::kInvalidRequest);
    }
  }
  std::sort(scoped_node_indices.begin(), scoped_node_indices.end());
  if (std::adjacent_find(scoped_node_indices.begin(), scoped_node_indices.end()) !=
      scoped_node_indices.end()) {
    return reject<void>("operation repeats an authored node scope",
                        "program_execution_scope_duplicate", ErrorCode::kInvalidRequest);
  }
  const auto synchronized =
      std::find_if(capture.table->named_selections.begin(), capture.table->named_selections.end(),
                   [&](const auto& selection) {
                     return selection.execution_policy == ProgramExecutionPolicy::kCommonStart &&
                            selection.node_indices == scoped_node_indices;
                   });
  if (intent.coordination_policy == OperationCoordinationPolicy::kCoordinatedAll) {
    if (synchronized == capture.table->named_selections.end()) {
      return reject<void>("coordinated operation is not an exact authored synchronized group",
                          "program_execution_synchronization_mismatch", ErrorCode::kInvalidRequest);
    }
  } else if (std::any_of(capture.table->named_selections.begin(),
                         capture.table->named_selections.end(), [&](const auto& selection) {
                           return selection.execution_policy !=
                                      ProgramExecutionPolicy::kIndependent &&
                                  std::any_of(scoped_node_indices.begin(),
                                              scoped_node_indices.end(), [&](const auto index) {
                                                return std::binary_search(
                                                    selection.node_indices.begin(),
                                                    selection.node_indices.end(), index);
                                              });
                         })) {
    return reject<void>("coordinated Program motion cannot be dispatched independently",
                        "program_execution_synchronization_required", ErrorCode::kInvalidRequest);
  }
  return Result<void>::Ok();
}

Result<void> ResourceAddressedExecutionKernel::release_program_run_stop_fence(
    const ProgramRunId& program_run_id, const StopId& stop_id,
    const StopGeneration stop_generation) {
  if (program_run_id.empty() || stop_id.empty() || !stop_generation.nonzero()) {
    return reject<void>("Program-run Stop fence identity is invalid",
                        "program_run_stop_fence_identity_invalid", ErrorCode::kInvalidRequest);
  }
  // Wait for an in-flight ordinary admission (bounded, no Stop dependency):
  // its child is then visible to the unsettled-child check below. Stop never
  // takes this lock, so waiting here cannot delay it.
  std::lock_guard admission_lock(*admission_mutex_);
  const auto parent = cell_admission_owner_->held_claim(CellAdmissionClaimKindDTO::kProgramCycle);
  if (parent.has_value() && parent->intent.owner_transaction_id == program_run_id.value() &&
      !cell_admission_owner_->unsettled_program_child_nodes(parent->claim_generation).empty()) {
    return reject<void>("Program transactions are still unresolved", "program_children_unsettled",
                        ErrorCode::kInvalidStateTransition);
  }
  std::lock_guard state_lock(state_mutex_);
  // A cell latch refuses Program resume.
  if (auto refusal = cell_latch_refusal_locked()) return Result<void>::Err(std::move(*refusal));
  const auto fence = program_run_stop_fences_.find(program_run_id);
  if (fence == program_run_stop_fences_.end()) {
    return Result<void>::Ok();
  }
  if (fence->stop_id != stop_id || fence->stop_generation != stop_generation) {
    return reject<void>("Program-run Stop fence identity changed", "program_run_stop_fence_stale",
                        ErrorCode::kInvalidStateTransition);
  }
  if (fence->selection_revision >= kMaximumKernelJsonInteger ||
      fence->selection_revision != fence->legacy_selection_revision ||
      !fence->pending_effects.empty()) {
    return reject<void>("a newer scoped selection superseded the whole-run Stop",
                        "program_run_stop_fence_stale", ErrorCode::kInvalidStateTransition);
  }
  if (pending_program_run_admission_ == std::optional<ProgramRunId>{program_run_id}) {
    return reject<void>("a pre-Stop Program-run admission is still draining",
                        "program_run_admission_in_flight", ErrorCode::kInvalidStateTransition);
  }
  if (!fence->releasable) {
    return reject<void>("Program-run Stop has not settled successfully",
                        "program_run_stop_fence_not_releasable",
                        ErrorCode::kInvalidStateTransition);
  }
  if (fence->node_states.empty()) {
    program_run_stop_fences_.erase(fence);
  } else {
    // Releasing the whole-run Stop resumes execution only. Motion holds are
    // kept: Ready work still needs Run and contained work still needs Resume.
    fence->stop_id.reset();
    ++fence->selection_revision;
  }
  return Result<void>::Ok();
}

Result<std::vector<std::uint32_t>>
ResourceAddressedExecutionKernel::resolve_program_selection_locked(
    const ProgramExecutionSelectionRequestDTO& request, const bool allow_node_release) const {
  using Indices = std::vector<std::uint32_t>;
  if (request.request_context.request_owner_epoch != config_.request_owner_epoch ||
      !program_cycle_claim_run_.has_value() || !program_cycle_claim_run_->capture.has_value() ||
      !program_cycle_claim_run_->capture->table ||
      request.program_run_id != program_cycle_claim_run_->program_run_id ||
      request.program_runner_owner_epoch != program_cycle_claim_run_->program_runner_owner_epoch ||
      request.claim_generation != program_cycle_claim_run_->claim_generation) {
    return reject<Indices>("Program selection does not name the current captured owner/claim",
                           "program_selection_stale", ErrorCode::kInvalidStateTransition);
  }
  const auto& table = *program_cycle_claim_run_->capture->table;
  if (request.node_instance_id.has_value()) {
    if (!allow_node_release || !request.group_ids.empty()) {
      return reject<Indices>("Exact node selection is exclusive to individual hold release",
                             "program_selection_invalid", ErrorCode::kInvalidRequest);
    }
    const auto node = std::lower_bound(
        table.nodes.begin(), table.nodes.end(), *request.node_instance_id,
        [](const auto& row, const auto& id) { return row.instance_id < id; });
    if (node == table.nodes.end() || node->instance_id != *request.node_instance_id) {
      return reject<Indices>("Program node is not present in the captured Program",
                             "program_selection_unknown", ErrorCode::kInvalidRequest);
    }
    return Result<Indices>::Ok({static_cast<std::uint32_t>(node - table.nodes.begin())});
  }
  if (request.group_ids.empty() || request.group_ids.size() > kMaxProgramExecutionGroups) {
    return reject<Indices>("Program selection must name a bounded nonempty group list",
                           "program_selection_invalid", ErrorCode::kInvalidRequest);
  }
  std::vector<bool> selected(table.nodes.size(), false);
  for (const auto& group : request.group_ids) {
    if (group == "all") {
      std::fill(selected.begin(), selected.end(), true);
      continue;
    }
    const auto found =
        std::lower_bound(table.named_selections.begin(), table.named_selections.end(), group,
                         [](const auto& row, const auto& id) { return row.group_id < id; });
    if (found == table.named_selections.end() || found->group_id != group) {
      return reject<Indices>("Program selection is not present in the captured Program",
                             "program_selection_unknown", ErrorCode::kInvalidRequest);
    }
    for (const auto index : found->node_indices) selected[index] = true;
  }
  Indices indices;
  for (std::size_t index = 0; index < selected.size(); ++index)
    if (selected[index]) indices.push_back(static_cast<std::uint32_t>(index));
  return Result<Indices>::Ok(std::move(indices));
}

bool ResourceAddressedExecutionKernel::program_node_effectful_locked(
    const ProgramRunId& program_run_id, const std::size_t index) const {
  if (!program_cycle_claim_run_.has_value() || !program_cycle_claim_run_->capture.has_value() ||
      program_cycle_claim_run_->program_run_id != program_run_id ||
      !program_cycle_claim_run_->capture->table ||
      index >= program_cycle_claim_run_->capture->table->nodes.size())
    return true;
  const auto& effects = program_cycle_claim_run_->capture->table->nodes[index].target_effects;
  return std::any_of(effects.begin(), effects.end(),
                     [](const auto& effect) { return effect.effectful; });
}

ProgramExecutionScopeStateDTO ResourceAddressedExecutionKernel::program_execution_scope_locked(
    const ProgramRunStopFence& fence) const {
  const auto& claim = *program_cycle_claim_run_;
  ProgramExecutionScopeStateDTO result{
      config_.request_owner_epoch, claim.program_runner_owner_epoch, claim.claim_generation,
      claim.program_run_id,        fence.selection_revision,         {},
      fence.pending_effects, program_selection_release_block_reason_locked(fence)};
  if (fence.held_by_rule) result.stopped_by_rule_id = fence.held_by_rule->value();
  for (std::size_t index = 0U; index < fence.node_states.size(); ++index) {
    if (fence.node_states[index] != ProgramNodeHoldStateDTO::kRunning) {
      result.held_nodes.push_back({static_cast<std::uint32_t>(index), fence.node_states[index]});
    }
  }
  return result;
}

std::optional<ProgramSelectionReleaseBlockReasonDTO>
ResourceAddressedExecutionKernel::program_selection_release_block_reason_locked(
    const ProgramRunStopFence& fence) const {
  // Keep this order aligned with the atomic Run/Resume refusal below. The
  // scope projection is advisory; release_program_selection re-evaluates this
  // same predicate while holding the Kernel state lock before changing holds.
  if (motion_admission_closed_locked())
    return ProgramSelectionReleaseBlockReasonDTO::kCellMotionAdmissionClosed;
  if (motion_admission_unreconciled_)
    return ProgramSelectionReleaseBlockReasonDTO::kCellMotionAdmissionUnreconciled;
  if (pending_program_run_admission_ == fence.program_run_id)
    return ProgramSelectionReleaseBlockReasonDTO::kProgramRunAdmissionPending;
  if (fence.stop_id.has_value())
    return ProgramSelectionReleaseBlockReasonDTO::kProgramHaltActive;
  return std::nullopt;
}

Result<ProgramExecutionScopeStateDTO> ResourceAddressedExecutionKernel::get_program_execution_scope(
    const ProgramRunId& program_run_id) const {
  std::lock_guard state_lock(state_mutex_);
  const auto fence =
      program_run_stop_fences_.find(program_run_id);
  if (!program_cycle_claim_run_.has_value() || !program_cycle_claim_run_->capture.has_value() ||
      program_cycle_claim_run_->program_run_id != program_run_id ||
      fence == program_run_stop_fences_.end()) {
    return reject<ProgramExecutionScopeStateDTO>("Program execution scope is not current",
                                                 "program_execution_scope_unavailable",
                                                 ErrorCode::kInvalidStateTransition);
  }
  return Result<ProgramExecutionScopeStateDTO>::Ok(program_execution_scope_locked(*fence));
}

Result<ProgramExecutionSelectionAcceptedDTO>
ResourceAddressedExecutionKernel::stop_program_selection(
    const ProgramExecutionSelectionRequestDTO& request) {
  const auto accepted_at_ns = clock_.now();
  StopWorkHandoff handoff;
  const ScopeExit publish([&] { publish_stop_work(handoff); });
  std::lock_guard state_lock(state_mutex_);
  return stop_program_selection_locked(request, handoff, accepted_at_ns);
}

Result<ProgramExecutionSelectionAcceptedDTO>
ResourceAddressedExecutionKernel::stop_program_selection_locked(
    const ProgramExecutionSelectionRequestDTO& request, StopWorkHandoff& handoff,
    const TimePoint accepted_at_ns) {
  auto selection = resolve_program_selection_locked(request);
  if (!selection.ok()) {
    const MalformedStopIngressDTO malformed{
        request.request_context.request_id, config_.request_owner_epoch, request.attribution,
        "/api/execution/stops", "recognized_stop_payload_invalid"};
    auto stopped = accept_stop_locked(EmergencyStopProjectedIntentDTO{std::monostate{}}, handoff,
                                      accepted_at_ns, nullptr, &malformed);
    if (!stopped.ok()) return Result<ProgramExecutionSelectionAcceptedDTO>::Err(stopped.error());
    return Result<ProgramExecutionSelectionAcceptedDTO>::Ok(
        {std::nullopt, {}, {}, std::move(stopped.value()), selection.error()});
  }
  auto fence =
      program_run_stop_fences_.find(request.program_run_id);
  // A resolved captured selection always has its fence installed. If that
  // invariant is lost, recognized Stop must still contain through the
  // existing conservative path rather than return before taking effect.
  if (fence == program_run_stop_fences_.end()) {
    const MalformedStopIngressDTO malformed{
        request.request_context.request_id, config_.request_owner_epoch, request.attribution,
        "/api/execution/stops", "recognized_stop_payload_invalid"};
    auto stopped = accept_stop_locked(EmergencyStopProjectedIntentDTO{std::monostate{}}, handoff,
                                      accepted_at_ns, nullptr, &malformed);
    if (!stopped.ok()) return Result<ProgramExecutionSelectionAcceptedDTO>::Err(stopped.error());
    return Result<ProgramExecutionSelectionAcceptedDTO>::Ok(
        {std::nullopt,
         {},
         {},
         std::move(stopped.value()),
         error("program_selection_owner_missing", "Program selection owner is missing",
               "program_selection_owner_missing")});
  }
  const auto& table = *program_cycle_claim_run_->capture->table;
  const auto replay = stop_replay_ledger_.find(request.request_context.request_id);
  if (replay != stop_replay_ledger_.end() ||
      ordinary_request_id_retained_locked(request.request_context.request_id) ||
      emergency_stop_reserve_.find(request.request_context.request_id.value()).has_value()) {
    const auto* prior = replay == stop_replay_ledger_.end()
                            ? nullptr
                            : std::get_if<ProgramExecutionSelectionRequestDTO>(&replay->intent);
    if (prior != nullptr && *prior == request) {
      const auto& receipt = std::get<StopReplayEntry::SelectionReceipt>(replay->accepted);
      return Result<ProgramExecutionSelectionAcceptedDTO>::Ok(
          {program_execution_scope_locked(*fence), selection.value(),
           receipt.widened_node_indices, receipt.fresh_containment, std::nullopt,
           RequestReplayStatusDTO::kReplayedExact});
    }
    const MalformedStopIngressDTO malformed{
        request.request_context.request_id, config_.request_owner_epoch, request.attribution,
        "/api/execution/stops", "recognized_stop_payload_invalid"};
    auto stopped = accept_stop_locked(EmergencyStopProjectedIntentDTO{std::monostate{}}, handoff,
                                      accepted_at_ns, nullptr, &malformed);
    if (!stopped.ok()) return Result<ProgramExecutionSelectionAcceptedDTO>::Err(stopped.error());
    return Result<ProgramExecutionSelectionAcceptedDTO>::Ok(
        {program_execution_scope_locked(*fence),
         selection.value(),
         {},
         std::move(stopped.value()),
         error("request_id_conflict",
               "Program Stop identity was reused with a different selection",
               "request_id_conflict")});
  }
  // Exact retained retries above are read-only even at the counter limit.
  // New Stop cannot be refused for exhausted selection bookkeeping: use
  // existing cell-wide containment, whose admission closure also fences any
  // late Execute. Resume already refuses this exhausted revision.
  if (fence->selection_revision >= kMaximumKernelJsonInteger) {
    const MalformedStopIngressDTO malformed{
        request.request_context.request_id, config_.request_owner_epoch, request.attribution,
        "/api/execution/stops", "recognized_stop_payload_invalid"};
    auto stopped = accept_stop_locked(EmergencyStopProjectedIntentDTO{std::monostate{}}, handoff,
                                      accepted_at_ns, nullptr, &malformed);
    if (!stopped.ok()) return Result<ProgramExecutionSelectionAcceptedDTO>::Err(stopped.error());
    return Result<ProgramExecutionSelectionAcceptedDTO>::Ok(
        {program_execution_scope_locked(*fence),
         selection.value(),
         {},
         std::move(stopped.value()),
         error("program_selection_revision_exhausted", "Program selection revision is exhausted",
               "program_selection_revision_exhausted")});
  }
  std::vector<bool> selected(table.nodes.size(), false);
  for (const auto index : selection.value()) selected[index] = true;
  const auto node_index = [&](const ProgramNodeExecutionScopeDTO& scope) {
    const auto node =
        std::lower_bound(table.nodes.begin(), table.nodes.end(), scope.node_instance_id,
                         [](const auto& row, const auto& id) { return row.instance_id < id; });
    return static_cast<std::size_t>(node - table.nodes.begin());
  };
  const auto intersects = [&](const auto& operation) {
    if (operation.manual_handover &&
        operation.manual_handover->capture.program_run_id == request.program_run_id) {
      if (std::find(request.group_ids.begin(), request.group_ids.end(), "all") !=
          request.group_ids.end())
        return true;
      return std::any_of(
          operation.manual_handover->producer_node_indices.begin(),
          operation.manual_handover->producer_node_indices.end(),
          [&](const auto index) { return index < selected.size() && selected[index]; });
    }
    return std::any_of(operation.intent.participants.begin(),
                       operation.intent.participants.end(), [&](const auto& row) {
                         return row.execution_scope.has_value() &&
                                node_index(*row.execution_scope) < selected.size() &&
                                selected[node_index(*row.execution_scope)];
                       });
  };
  // Coordinated work is indivisible. A node can own several effects, so
  // retain the closure, not just the first operation encountered.
  bool widened = true;
  while (widened) {
    widened = false;
    for (const auto& [id, operation] : active_operations_) {
      if (operation->state.program_run_id != request.program_run_id ||
          operation->intent.coordination_policy != OperationCoordinationPolicy::kCoordinatedAll)
        continue;
      const auto& participants = operation->intent.participants;
      if (!intersects(*operation)) continue;
      for (const auto& row : participants) {
        if (!row.execution_scope.has_value()) continue;
        const auto index = node_index(*row.execution_scope);
        if (index < selected.size() && !selected[index]) {
          selected[index] = true;
          widened = true;
        }
      }
    }
  }
  ProgramExecutionSelectionAcceptedDTO accepted{
      std::nullopt, selection.value(), {}, std::nullopt};
  for (std::size_t index = 0U; index < selected.size(); ++index) {
    if (!selected[index]) continue;
    if (!std::binary_search(selection.value().begin(), selection.value().end(), index))
      accepted.widened_node_indices.push_back(static_cast<std::uint32_t>(index));
    if (fence->node_states[index] == ProgramNodeHoldStateDTO::kRunning)
      fence->node_states[index] = ProgramNodeHoldStateDTO::kHeld;
  }
  ++fence->selection_revision;
  fence_held_prepared_program_commands_locked(request.program_run_id);
  // The held state is now authoritative, under the same lock used by final
  // Execute. No late admission can cross the following physical capture.
  std::vector<StopOperationTargetDTO> fresh;
  for (const auto& [id, operation] : active_operations_) {
    if (operation->state.program_run_id != request.program_run_id &&
        (!operation->manual_handover ||
         operation->manual_handover->capture.program_run_id != request.program_run_id))
      continue;
    if (!intersects(*operation) ||
        (terminal(operation->state.state) && !operation->containment_effect_uncertain()))
      continue;
    const bool already_contained = std::any_of(
        stop_transactions_.begin(), stop_transactions_.end(), [&](const auto& transaction) {
          if (transaction == nullptr || !transaction->active || transaction->terminal ||
              transaction->stop_id != operation->state.active_stop_id ||
              transaction->stop_generation != operation->state.active_stop_generation ||
              !std::binary_search(transaction->operations.begin(), transaction->operations.end(),
                                  id))
            return false;
          return std::all_of(operation->participants.begin(), operation->participants.end(),
                             [&](const auto& participant) {
                               return transaction->participant_ids.empty() ||
                                      std::binary_search(transaction->participant_ids.begin(),
                                                         transaction->participant_ids.end(),
                                                         participant.slot_owner.participant_id) ||
                                      (operation->intent.coordination_policy ==
                                           OperationCoordinationPolicy::kCoordinatedAll &&
                                       !participant.prepared_dispatch_released);
                             });
        });
    if (!already_contained)
      fresh.push_back({id, operation->state.operation_revision,
                       operation->state.active_stop_generation.value_or(StopGeneration(0U))});
  }
  if (!fresh.empty()) {
    auto stopped = accept_stop_locked(EmergencyStopProjectedIntentDTO{StopIntentDTO{
                                          request.request_context, std::move(fresh),
                                          "Program node selection", request.attribution}},
                                      handoff, accepted_at_ns);
    if (!stopped.ok()) return Result<ProgramExecutionSelectionAcceptedDTO>::Err(stopped.error());
    accepted.fresh_containment = std::move(stopped.value());
    if (accepted.fresh_containment->resolution_disposition ==
        StopResolutionDispositionDTO::kPromotedToFull) {
      for (std::size_t index = 0U; index < selected.size(); ++index) {
        if (!selected[index])
          accepted.widened_node_indices.push_back(static_cast<std::uint32_t>(index));
        selected[index] = true;
        if (fence->node_states[index] == ProgramNodeHoldStateDTO::kRunning)
          fence->node_states[index] = ProgramNodeHoldStateDTO::kHeld;
      }
      std::sort(accepted.widened_node_indices.begin(), accepted.widened_node_indices.end());
    }
  }
  // Adopt exact current Stop ownership; never resubmit an already covered
  // operation and never use history absence as evidence of rest.
  for (const auto& [id, operation] : active_operations_) {
    if (operation->state.program_run_id != request.program_run_id ||
        !operation->state.active_stop_id.has_value() ||
        !operation->state.active_stop_generation.has_value())
      continue;
    const auto& stop_id = *operation->state.active_stop_id;
    const auto generation = *operation->state.active_stop_generation;
    const auto resident =
        std::find_if(stop_transactions_.begin(), stop_transactions_.end(), [&](const auto& row) {
          return row != nullptr && row->active && row->stop_id == stop_id &&
                 row->stop_generation == generation;
        });
    const auto terminal_stop = operation->terminal_stop_result();
    for (const auto& participant : operation->state.participants) {
      if (!participant.execution_scope.has_value()) continue;
      {
        const auto* origin = &*participant.execution_scope;
        const auto index = node_index(*origin);
        if (index >= selected.size() || !selected[index]) continue;
        if (terminal_stop.has_value()) {
          const auto disposition = *terminal_stop;
          if (disposition != StopCertificateResultDTO::kSucceededNominal &&
              disposition != StopCertificateResultDTO::kSucceededEscalated)
            fence->node_states[index] = ProgramNodeHoldStateDTO::kFaulted;
          continue;
        }
        const auto live_participant = std::find_if(
            operation->participants.begin(), operation->participants.end(), [&](const auto& row) {
              return row.slot_owner.participant_id == participant.participant_id;
            });
        if (live_participant != operation->participants.end() &&
            live_participant->non_motion_transaction()) continue;
        const bool never_released_peer = live_participant != operation->participants.end() &&
                                         operation->intent.coordination_policy ==
                                             OperationCoordinationPolicy::kCoordinatedAll &&
                                         !live_participant->prepared_dispatch_released;
        const bool covered =
            resident != stop_transactions_.end() && !(*resident)->terminal &&
            (std::binary_search((*resident)->operations.begin(), (*resident)->operations.end(),
                                id)) &&
            ((*resident)->participant_ids.empty() ||
             std::binary_search((*resident)->participant_ids.begin(),
                                (*resident)->participant_ids.end(), participant.participant_id) ||
             never_released_peer);
        if (!covered) {
          // Neither exact settlement nor an active containing Stop is known.
          fence->node_states[index] = ProgramNodeHoldStateDTO::kFaulted;
          continue;
        }
        const auto retained = std::find_if(
            fence->pending_effects.begin(), fence->pending_effects.end(), [&](const auto& row) {
              return row.node_index == index && row.operation_id == id &&
                     row.participant_id == participant.participant_id && row.stop_id == stop_id &&
                     row.stop_generation == generation;
            });
        if (retained == fence->pending_effects.end()) {
          fence->pending_effects.push_back({static_cast<std::uint32_t>(index), id,
                                            participant.participant_id, origin->node_attempt_id,
                                            stop_id, generation});
        }
        if (fence->node_states[index] != ProgramNodeHoldStateDTO::kFaulted)
          fence->node_states[index] = ProgramNodeHoldStateDTO::kStopping;
      }
    }
  }
  std::sort(fence->pending_effects.begin(), fence->pending_effects.end(),
            [](const auto& a, const auto& b) {
              return std::tie(a.node_index, a.operation_id, a.participant_id) <
                     std::tie(b.node_index, b.operation_id, b.participant_id);
            });
  // Reuse the same bounded Stop receipt ledger for physical and selection
  // requests; never retain an entire copied scope snapshot per request.
  StopReplayEntry receipt{request,
                          StopReplayEntry::SelectionReceipt{accepted.widened_node_indices,
                                                            accepted.fresh_containment}};
  const auto physical_receipt = stop_replay_ledger_.find(request.request_context.request_id);
  if (physical_receipt != stop_replay_ledger_.end())
    *physical_receipt = std::move(receipt);
  else {
    if (stop_replay_ledger_.size() >= config_.request_replay_capacity)
      stop_replay_ledger_.pop_front();
    stop_replay_ledger_.push_back(std::move(receipt));
  }
  accepted.state = program_execution_scope_locked(*fence);
  return Result<ProgramExecutionSelectionAcceptedDTO>::Ok(std::move(accepted));
}

void ResourceAddressedExecutionKernel::reduce_program_selection_stop_locked(
    const StopCertificateDTO& certificate) {
  const bool succeeded = certificate.result == StopCertificateResultDTO::kSucceededNominal ||
                         certificate.result == StopCertificateResultDTO::kSucceededEscalated;
  const auto matches = [&](const auto& effect) {
    return effect.stop_id == certificate.stop_id &&
           effect.stop_generation == certificate.stop_generation;
  };
  for (auto& fence : program_run_stop_fences_) {
    bool selection_changed = false;
    for (auto first = fence.pending_effects.begin(); first != fence.pending_effects.end();) {
      auto last = std::find_if(first, fence.pending_effects.end(), [&](const auto& row) {
        return row.node_index != first->node_index;
      });
      if (std::any_of(first, last, matches)) {
        auto& state = fence.node_states[first->node_index];
        if (!succeeded) {
          selection_changed |= terminal_program_fault_changes_selection(state);
          state = ProgramNodeHoldStateDTO::kFaulted;
        } else if (state != ProgramNodeHoldStateDTO::kFaulted && std::all_of(first, last, matches))
          state = ProgramNodeHoldStateDTO::kHeld;
      }
      first = last;
    }
    std::erase_if(fence.pending_effects, matches);
    if (selection_changed)
      advance_terminal_program_fault_selection(
          fence.selection_revision, fence.legacy_selection_revision, kMaximumKernelJsonInteger);
  }
}

Result<ProgramExecutionScopeStateDTO> ResourceAddressedExecutionKernel::start_program_selection(
    const ProgramExecutionSelectionRequestDTO& request,
    const std::uint64_t expected_selection_revision) {
  if (request.node_instance_id.has_value()) {
    return reject<ProgramExecutionScopeStateDTO>(
        "Exact node selection is exclusive to individual hold release",
        "program_selection_invalid", ErrorCode::kInvalidRequest);
  }
  // Run is the same release as Resume (see release_program_selection).
  return release_program_selection(request, expected_selection_revision);
}

Result<ProgramExecutionScopeStateDTO> ResourceAddressedExecutionKernel::release_program_selection(
    const ProgramExecutionSelectionRequestDTO& request,
    const std::uint64_t expected_selection_revision) {
  // Run/Resume waits for an in-flight ordinary admission instead of refusing
  // a timing collision: the admitted child is then visible to the snapshot
  // below. Same admission -> state order as Execute; Stop never takes it.
  std::lock_guard admission_lock(*admission_mutex_);
  // Snapshot under ordinary ordering, before taking Stop's state lock. No
  // receipt/journal I/O or ledger lock is added to that critical section.
  const auto child_nodes =
      cell_admission_owner_->unsettled_program_child_nodes(request.claim_generation);
  std::lock_guard state_lock(state_mutex_);
  // A cell latch refuses Program run and resume.
  if (auto refusal = cell_latch_refusal_locked())
    return Result<ProgramExecutionScopeStateDTO>::Err(std::move(*refusal));
  // Resume waits while a rule holds the Program's scope (`rule_holds`
  // with its `rule_id`), here as in readiness.
  if (program_cycle_claim_run_ && program_cycle_claim_run_->capture)
    for (const auto& registration : registrations_)
      if (registration.binding.value().program_binding_id ==
          program_cycle_claim_run_->capture->program_binding_id)
        if (auto hold = global_io_rule_command_refusal_locked(registration.binding.value()))
          return Result<ProgramExecutionScopeStateDTO>::Err(std::move(*hold));
  auto selection = resolve_program_selection_locked(request, true);
  if (!selection.ok()) return Result<ProgramExecutionScopeStateDTO>::Err(selection.error());
  auto fence =
      program_run_stop_fences_.find(request.program_run_id);
  if (fence == program_run_stop_fences_.end() ||
      fence->selection_revision != expected_selection_revision ||
      fence->selection_revision >= kMaximumKernelJsonInteger) {
    return reject<ProgramExecutionScopeStateDTO>(
        "Program selection changed after Resume was prepared", "program_selection_revision_stale",
        ErrorCode::kInvalidStateTransition);
  }
  // A whole-run Stop (Program Halt) governs execution; only releasing it
  // (Execute) ends it. Resume releases motion holds and never ends a Halt.
  if (const auto blocker = program_selection_release_block_reason_locked(*fence);
      blocker.has_value()) {
    std::string message;
    switch (*blocker) {
      case ProgramSelectionReleaseBlockReasonDTO::kCellMotionAdmissionClosed:
        message = "Cell motion admission is closed; Program Run or Resume remains blocked";
        break;
      case ProgramSelectionReleaseBlockReasonDTO::kCellMotionAdmissionUnreconciled:
        message = "Cell motion admission is unreconciled; Program Run or Resume remains blocked";
        break;
      case ProgramSelectionReleaseBlockReasonDTO::kProgramRunAdmissionPending:
        message = "A Program-run admission is pending; Program Run or Resume remains blocked";
        break;
      case ProgramSelectionReleaseBlockReasonDTO::kProgramHaltActive:
        message = "Program Halt is still active; release its fence before Run or Resume";
        break;
    }
    const auto error_code = std::string(to_string(ErrorCode::kInvalidStateTransition));
    return Result<ProgramExecutionScopeStateDTO>::Err(ErrorDTO{
        error_code, std::move(message),
        {{"reason", "program_selection_not_releasable"},
         {"selection_release_block_reason",
          std::string(program_selection_release_block_reason_token(*blocker))}}});
  }
  // Resume releases only current holds. An overlapping selection can also
  // contain a branch already resumed and executing a fresh operation; that
  // work is neither a release target nor a reason to block the remaining held
  // branches. Resolve the effective subset under the same revision fence.
  std::erase_if(selection.value(), [&](const auto index) {
    return fence->node_states[index] == ProgramNodeHoldStateDTO::kRunning;
  });
  if (selection.value().empty()) {
    return reject<ProgramExecutionScopeStateDTO>("Selected scope is not held",
                                                 "program_selection_not_releasable",
                                                 ErrorCode::kInvalidStateTransition);
  }
  // Run and Resume are one release: never-started (Ready) and stopped (Held)
  // work are both released; Stopping or Faulted work is not.
  for (const auto index : selection.value()) {
    if (fence->node_states[index] != ProgramNodeHoldStateDTO::kHeld &&
        fence->node_states[index] != ProgramNodeHoldStateDTO::kReady) {
      return reject<ProgramExecutionScopeStateDTO>(
          "Run/Resume accepts only Ready work and settled ordinary-Stop holds",
          "program_selection_not_releasable", ErrorCode::kInvalidStateTransition);
    }
  }
  const auto& nodes = program_cycle_claim_run_->capture->table->nodes;
  for (const auto& child : child_nodes) {
    const auto node = std::lower_bound(
        nodes.begin(), nodes.end(), child,
        [](const auto& row, const auto& id) { return row.instance_id.value() < id; });
    if (child.empty() || node == nodes.end() || node->instance_id.value() != child ||
        std::binary_search(selection.value().begin(), selection.value().end(),
                           static_cast<std::uint32_t>(node - nodes.begin()))) {
      return reject<ProgramExecutionScopeStateDTO>(
          "selected Program transactions are still unresolved", "program_children_unsettled",
          ErrorCode::kInvalidStateTransition);
    }
  }
  const auto scope_selected = [&](const ProgramNodeExecutionScopeDTO& scope) {
    const auto node =
        std::lower_bound(nodes.begin(), nodes.end(), scope.node_instance_id,
                         [](const auto& row, const auto& id) { return row.instance_id < id; });
    return std::binary_search(selection.value().begin(), selection.value().end(),
                              node - nodes.begin());
  };
  for (const auto& [id, operation] : active_operations_) {
    if (operation->state.program_run_id != request.program_run_id) continue;
    for (const auto& participant : operation->intent.participants) {
      if (!participant.execution_scope.has_value()) continue;
      if (scope_selected(*participant.execution_scope)) {
        return reject<ProgramExecutionScopeStateDTO>(
            "Selected operation requires exact recovery/retirement before release",
            "program_selection_recovery_required", ErrorCode::kInvalidStateTransition);
      }
    }
  }
  // Ordinary Run/Resume: the released nodes' bound resources are the
  // candidate scope; each later operation is rechecked at its own admission.
  if (auto refusal =
          operator_alarm_attention_refusal_locked(program_selection_resources_locked(selection.value()))) {
    return Result<ProgramExecutionScopeStateDTO>::Err(std::move(*refusal));
  }
  // All selected eligibility was checked before any node is released. An
  // unrelated branch may keep executing; there is no per-group lock to clear.
  for (const auto index : selection.value())
    fence->node_states[index] = ProgramNodeHoldStateDTO::kRunning;
  ++fence->selection_revision;
  fence->held_by_rule.reset();
  return Result<ProgramExecutionScopeStateDTO>::Ok(program_execution_scope_locked(*fence));
}

Result<StopAcceptedDTO> ResourceAddressedExecutionKernel::recognized_malformed_stop(
    const MalformedStopIngressDTO& ingress) {
  return accept_stop(EmergencyStopProjectedIntentDTO{std::monostate{}}, nullptr, &ingress);
}

Result<RuntimeScaleAcceptedDTO> ResourceAddressedExecutionKernel::set_runtime_scale_impl(
    const RuntimeScaleRequestDTO& request, const std::uint64_t value_sequence) {
  if (request.request_context.request_id.empty() ||
      request.request_context.request_owner_epoch.empty() ||
      request.request_context.source_label.empty() || request.operation_id.empty() ||
      !request.expected_operation_revision.nonzero() || !std::isfinite(request.scale) ||
      request.scale < 0.0 || request.scale > 1.0 || !request.attribution.locally_valid()) {
    return reject<RuntimeScaleAcceptedDTO>("runtime-scale request is malformed",
                                           "runtime_scale_request_invalid",
                                           ErrorCode::kInvalidRequest);
  }
  if (request.request_context.request_owner_epoch != config_.request_owner_epoch) {
    return Result<RuntimeScaleAcceptedDTO>::Err(
        error("stale_request_owner_epoch", "runtime-scale request names a stale Kernel owner epoch",
              "stale_request_owner_epoch"));
  }

  struct ScaleEffectTarget final {
    IResolvedParticipantExecutionEngine* engine{};
    ResolvedRuntimeScaleCommandDTO command;
  };
  std::vector<ScaleEffectTarget> effect_targets;
  RuntimeScaleAcceptedDTO accepted{request.request_context.request_id,
                                   RequestReplayStatusDTO::kAcceptedNew,
                                   request.operation_id,
                                   request.expected_operation_revision,
                                   request.scale,
                                   RuntimeScaleGeneration(0U),
                                   0};
  {
    OperationCommitGuard commit(*this, request.operation_id);
    std::unique_lock state_lock(state_mutex_);
    if (emergency_stop_reserve_.find(request.request_context.request_id.value()).has_value()) {
      return Result<RuntimeScaleAcceptedDTO>::Err(
          error("request_id_conflict", "runtime-scale request ID is retained by emergency Stop",
                "request_id_conflict"));
    }
    const auto request_id = request.request_context.request_id;
    const bool used_elsewhere =
        replay_ledger_.contains(request_id) ||
        control_replay_ledger_.contains(request_id) ||
        stop_replay_ledger_.contains(request_id) ||
        runtime_scale_control_replay_ledger_.contains(request_id);
    if (used_elsewhere) {
      return Result<RuntimeScaleAcceptedDTO>::Err(
          error("request_id_conflict",
                "runtime-scale request ID was already used by another Kernel command",
                "request_id_conflict"));
    }
    auto* active_operation = find_active_operation_locked(request.operation_id);
    if (active_operation == nullptr) {
      return reject<RuntimeScaleAcceptedDTO>("runtime scale names a non-resident operation",
                                             "operation_not_resident",
                                             ErrorCode::kMissingReference);
    }
    // Like Stop, runtime scale resolves to the operation's current revision;
    // the caller's expected revision is informational.
    // Shared-start execution is removed: no coordinated operation has a
    // shared execution clock, so its runtime scaling stays refused.
    if (active_operation->participants.size() != 1U)
      return reject<RuntimeScaleAcceptedDTO>(
          "coordinated runtime scaling requires a released shared execution clock",
          "coordinated_runtime_scale_not_qualified", ErrorCode::kUnsupportedCapability);
    if (!active_operation->runtime_scale_status.has_value() ||
        std::any_of(active_operation->participants.begin(), active_operation->participants.end(),
                    [](const auto& participant) {
                      return participant.payload_kind != OperationParticipantPayloadKind::kMotion ||
                             !participant.binding.value().control_path ||
                             !participant.binding.value().control_path->runtime_scale_qualification;
                    })) {
      return reject<RuntimeScaleAcceptedDTO>(
          "the exact operation binding has no qualified runtime scaling",
          "runtime_scale_unsupported", ErrorCode::kUnsupportedCapability);
    }
    for (const auto& participant : active_operation->participants) {
      const auto availability = runtime_scale_availability_of(participant.engine);
      if (availability == RuntimeScaleMechanismAvailability::kLaneUnsupported) {
        return reject<RuntimeScaleAcceptedDTO>(
            "the participant's controller cannot apply a runtime scale",
            "runtime_scale_unsupported", ErrorCode::kUnsupportedCapability);
      }
      if (availability == RuntimeScaleMechanismAvailability::kControllerNotListening) {
        return reject<RuntimeScaleAcceptedDTO>(
            "the participant's controller is not listening for runtime scale input",
            kRuntimeScaleControllerNotListening, ErrorCode::kUnsupportedCapability);
      }
    }
    // OD-72: latest value wins. A speed change is never refused as busy or
    // stale: the caller's expected generation is informational, and this
    // operation's commit guard orders the acceptances (and their journal
    // writes) by generation; the value sequence orders their effects at the
    // participant.
    const auto current_generation =
        active_operation->runtime_scale_status->runtime_scale_generation;
    const auto state = active_operation->state.state;
    if (state != OperationStateKind::kPlanning && state != OperationStateKind::kValidating &&
        state != OperationStateKind::kPrepared && state != OperationStateKind::kDispatching &&
        state != OperationStateKind::kExecuting) {
      return reject<RuntimeScaleAcceptedDTO>(
          "runtime scale is invalid for the current operation state", "runtime_scale_state_invalid",
          ErrorCode::kInvalidStateTransition);
    }
    if (current_generation.value() == std::numeric_limits<std::uint64_t>::max()) {
      return reject<RuntimeScaleAcceptedDTO>("runtime-scale generation space is exhausted",
                                             "runtime_scale_generation_exhausted",
                                             ErrorCode::kInternalError);
    }

    const RuntimeScaleGeneration next_generation(current_generation.value() + 1U);
    accepted = RuntimeScaleAcceptedDTO{request.request_context.request_id,
                                       RequestReplayStatusDTO::kAcceptedNew,
                                       request.operation_id,
                                       active_operation->state.operation_revision,
                                       request.scale,
                                       next_generation,
                                       clock_.now()};
    auto accepted_status = *active_operation->runtime_scale_status;
    accepted_status.runtime_scale_generation = next_generation;
    accepted_status.requested_scale = request.scale;
    accepted_status.state = RuntimeScaleApplicationState::kPending;
    accepted_status.updated_at_ns = accepted.accepted_at_ns;
    for (auto& participant : accepted_status.participants) {
      participant.accepted_generation = next_generation;
      participant.commanded_scale = request.scale;
      participant.state = RuntimeScaleApplicationState::kPending;
      participant.error.reset();
    }
    auto accepted_state = active_operation->state;
    const auto prior_state = active_operation->state;
    accepted_state.runtime_scale_generation = next_generation;
    accepted_state.updated_at_ns = accepted.accepted_at_ns;
    stamp_operation_capture_locked(*active_operation, accepted_state);
    state_lock.unlock();
    auto durable_acceptance = attempt_journal_write([&] {
      return journal_.RecordRuntimeScaleAccepted(config_.request_owner_epoch, request, accepted,
                                                 accepted_state, accepted_status);
    });
    state_lock.lock();
    if (!durable_acceptance.ok()) {
      retain_operation_journal_write_fault_locked(
          "runtime_scale_accepted", durable_acceptance.error(), &accepted_state,
          request.request_context.request_id);
      return Result<RuntimeScaleAcceptedDTO>::Err(durable_acceptance.error());
    }
    active_operation = find_active_operation_locked(request.operation_id);
    if (active_operation == nullptr ||
        active_operation->state.journal_capture_order != prior_state.journal_capture_order ||
        active_operation->state.operation_revision != prior_state.operation_revision ||
        active_operation->state.active_stop_id != prior_state.active_stop_id)
      return Result<RuntimeScaleAcceptedDTO>::Ok(accepted);
    active_operation->state = std::move(accepted_state);
    active_operation->runtime_scale_status = std::move(accepted_status);
    effect_targets.reserve(active_operation->participants.size());
    for (std::size_t index = 0U; index < active_operation->participants.size(); ++index) {
      const auto& current_participant = active_operation->participants[index];
      const auto& binding = current_participant.binding.value();
      const auto& control = *binding.control_path;
      const auto& qualification = *control.runtime_scale_qualification;
      const auto& operation_participant = active_operation->state.participants[index];
      effect_targets.push_back(ScaleEffectTarget{
          current_participant.engine,
          ResolvedRuntimeScaleCommandDTO{request.operation_id,
                                         accepted.operation_revision,
                                         operation_participant.participant_id,
                                         binding.controllable_component_id,
                                         binding.command_target_id,
                                         binding.capability_id,
                                         control.control_domain_id,
                                         control.control_domain_revision,
                                         control.control_binding_id,
                                         control.control_binding_revision,
                                         operation_participant.command_id,
                                         operation_participant.command_generation,
                                         control.controller_topology_id,
                                         control.topology_revision,
                                         control.control_route_id,
                                         qualification.scaling_domain_id,
                                         qualification.coordinated_barrier_id,
                                         current_participant.resources,
                                         control.active_control_configuration_id,
                                         control.active_control_configuration_generation,
                                         current_generation,
                                         next_generation,
                                         request.scale,
                                         value_sequence}});
    }
    // Lifetime borrow of the captured engines until their calls return.
    if (!effect_targets.empty()) ++runtime_scale_effect_calls_in_flight_;
  }
  const ScopeExit release_effect_borrow([&] {
    if (effect_targets.empty()) return;
    std::lock_guard state_lock(state_mutex_);
    --runtime_scale_effect_calls_in_flight_;
  });

  const auto mark_failed = [&](IResolvedParticipantExecutionEngine* engine, ErrorDTO failure,
                               const char* reason) {
    OperationCommitGuard commit(*this, request.operation_id);
    std::unique_lock state_lock(state_mutex_);
    auto* active_operation = find_active_operation_locked(request.operation_id);
    if (active_operation == nullptr || !active_operation->runtime_scale_status.has_value() ||
        active_operation->state.operation_revision != accepted.operation_revision ||
        active_operation->runtime_scale_status->runtime_scale_generation !=
            accepted.runtime_scale_generation)
      return;
    auto status = *active_operation->runtime_scale_status;
    status.state = RuntimeScaleApplicationState::kFailed;
    status.updated_at_ns = clock_.now();
    const auto participant = active_operation->participant_for(engine);
    const auto row = std::find_if(
        status.participants.begin(), status.participants.end(), [&](const auto& candidate) {
          return participant != nullptr &&
                 candidate.participant_id == participant->slot_owner.participant_id;
        });
    if (row != status.participants.end()) {
      row->state = RuntimeScaleApplicationState::kFailed;
      row->error = std::move(failure);
    }
    (void)retain_runtime_scale_observation_locked(state_lock, *active_operation, std::move(status),
                                                  reason);
  };
  for (const auto& target : effect_targets) {
    const auto commanded = [&]() -> Result<ResolvedRuntimeScaleAcceptedDTO> {
      try {
        return target.engine->set_runtime_scale(target.command);
      } catch (...) {
        return Result<ResolvedRuntimeScaleAcceptedDTO>::Err(
            error(std::string(to_string(ErrorCode::kInvariantViolation)),
                  "participant runtime-scale outcome is unknown",
                  "runtime_scale_effect_outcome_unknown"));
      }
    }();
    if (!commanded.ok()) {
      mark_failed(target.engine, commanded.error(), "runtime_scale_effect_rejected");
      continue;
    }
    const auto& exact = commanded.value();
    const auto& resolved_command = target.command;
    const bool acceptance_matches =
        exact.operation_id == resolved_command.operation_id &&
        exact.operation_revision == resolved_command.operation_revision &&
        exact.participant_id == resolved_command.participant_id &&
        exact.controllable_component_id == resolved_command.controllable_component_id &&
        exact.command_target_id == resolved_command.command_target_id &&
        exact.capability_id == resolved_command.capability_id &&
        exact.control_domain_id == resolved_command.control_domain_id &&
        exact.control_domain_revision == resolved_command.control_domain_revision &&
        exact.control_binding_id == resolved_command.control_binding_id &&
        exact.control_binding_revision == resolved_command.control_binding_revision &&
        exact.controller_goal_id == resolved_command.controller_goal_id &&
        exact.controller_goal_generation == resolved_command.controller_goal_generation &&
        exact.controller_topology_id == resolved_command.controller_topology_id &&
        exact.topology_revision == resolved_command.topology_revision &&
        exact.control_route_id == resolved_command.control_route_id &&
        exact.scaling_domain_id == resolved_command.scaling_domain_id &&
        exact.coordinated_barrier_id == resolved_command.coordinated_barrier_id &&
        exact.resources == resolved_command.resources &&
        exact.active_control_configuration_id == resolved_command.active_control_configuration_id &&
        exact.active_control_configuration_generation ==
            resolved_command.active_control_configuration_generation &&
        exact.accepted_generation == resolved_command.requested_runtime_scale_generation &&
        std::abs(exact.commanded_scale - resolved_command.requested_scale) <= 1e-12;
    if (!acceptance_matches) {
      mark_failed(target.engine,
                  error(std::string(to_string(ErrorCode::kInvariantViolation)),
                        "participant engine returned mismatched runtime-scale acceptance",
                        "runtime_scale_acceptance_mismatch"),
                  "runtime_scale_acceptance_mismatch");
      continue;
    }
    const auto observed = target.engine->runtime_scale_status(resolved_command.operation_id,
                                                              resolved_command.control_binding_id);
    if (!observed.ok()) {
      mark_failed(target.engine, observed.error(), "runtime_scale_status_unavailable");
      continue;
    }
    (void)apply_runtime_scale_status(target.engine, observed.value());
  }
  return Result<RuntimeScaleAcceptedDTO>::Ok(std::move(accepted));
}

Result<RuntimeScaleControlAcceptedDTO> ResourceAddressedExecutionKernel::set_runtime_scale_control(
    const RuntimeScaleControlRequestDTO& request) {
  if (request.request_context.request_id.empty() ||
      request.request_context.request_owner_epoch.empty() ||
      request.request_context.source_label.empty() || request.selector.program_binding_id.empty() ||
      (request.selector.group_targets.empty() &&
       (request.selector.component_id.empty() || request.selector.command_target_id.empty())) ||
      !std::isfinite(request.scale) || request.scale < 0.0 || request.scale > 1.0 ||
      !request.attribution.locally_valid()) {
    return reject<RuntimeScaleControlAcceptedDTO>("runtime-scale control request is malformed",
                                                  "runtime_scale_control_request_invalid",
                                                  ErrorCode::kInvalidRequest);
  }
  if (request.request_context.request_owner_epoch != config_.request_owner_epoch) {
    return Result<RuntimeScaleControlAcceptedDTO>::Err(
        error("stale_request_owner_epoch",
              "runtime-scale control request names a stale Kernel owner epoch",
              "stale_request_owner_epoch"));
  }

  // Linearize the persistent setting with new operation admission: Execute
  // reads the setting (with its value sequence) and registers its operation
  // under this mutex, so an operation either is admitted later and seeded
  // with the new setting, or is already resident and collected below. Its
  // participant takes the value as its ramp target either way, ordered by the
  // value sequence (latest wins, OD-72), so a motion still preparing reads it
  // when it registers its ramp. Held only until the setting, the targets and
  // the replay entry are recorded; the effects run without it (audit 3.8).
  // Emergency Stop never takes this mutex and therefore never waits for this
  // path.
  std::unique_lock submit_lock(submission_mutex_);
  {
    std::lock_guard state_lock(state_mutex_);
    const auto replay =
        runtime_scale_control_replay_ledger_.find(request.request_context.request_id);
    if (replay != runtime_scale_control_replay_ledger_.end()) {
      if (replay->request != request) {
        return Result<RuntimeScaleControlAcceptedDTO>::Err(
            error("request_id_conflict",
                  "runtime-scale control request ID was reused with different intent",
                  "request_id_conflict"));
      }
      auto accepted = replay->accepted;
      accepted.replay_status = RequestReplayStatusDTO::kReplayedExact;
      return Result<RuntimeScaleControlAcceptedDTO>::Ok(std::move(accepted));
    }
    const auto request_id = request.request_context.request_id;
    const bool used_elsewhere =
        emergency_stop_reserve_.find(request_id.value()).has_value() ||
        replay_ledger_.contains(request_id) ||
        control_replay_ledger_.contains(request_id) ||
        stop_replay_ledger_.contains(request_id);
    if (used_elsewhere) {
      return Result<RuntimeScaleControlAcceptedDTO>::Err(
          error("request_id_conflict",
                "runtime-scale control request ID was already used by another Kernel command",
                "request_id_conflict"));
    }
    const auto resolved = resolve_runtime_scale_control_locked(request.selector);
    if (!resolved.ok()) {
      return Result<RuntimeScaleControlAcceptedDTO>::Err(resolved.error());
    }
    const auto setting =
        std::find_if(runtime_scale_control_settings_.begin(), runtime_scale_control_settings_.end(),
                     [&](const RuntimeScaleControlSetting& candidate) {
                       return candidate.resolution.same_control_scope(resolved.value());
                     });
    // OD-72: latest value wins; the caller's expected generation is
    // informational and never refuses a speed change.
    const auto current_generation = setting == runtime_scale_control_settings_.end()
                                        ? RuntimeScaleControlGeneration(0U)
                                        : setting->generation;
    if (current_generation.value() == std::numeric_limits<std::uint64_t>::max()) {
      return reject<RuntimeScaleControlAcceptedDTO>(
          "runtime-scale control generation space is exhausted",
          "runtime_scale_control_generation_exhausted", ErrorCode::kInternalError);
    }

    // Validate the exact scope setting plus the per-member effective settings
    // as one bounded atomic update. Aggregate selectors retain their own
    // generation for replay while writing through to the singular settings
    // used to seed later independent motions.
    std::vector<RuntimeScaleControlScope> target_settings;
    target_settings.reserve(resolved.value().members.size() + 1U);
    target_settings.push_back(resolved.value());
    if (resolved.value().members.size() > 1U) {
      for (const auto& member : resolved.value().members) {
        target_settings.push_back(RuntimeScaleControlScope{member.selector, {member}});
      }
    }
    auto projected_setting_count = runtime_scale_control_settings_.size();
    auto projected_memberships = std::accumulate(
        runtime_scale_control_settings_.begin(), runtime_scale_control_settings_.end(),
        std::size_t{0U}, [](const std::size_t total, const RuntimeScaleControlSetting& retained) {
          return total + retained.resolution.members.size();
        });
    for (const auto& target : target_settings) {
      const auto same_scope = std::find_if(
          runtime_scale_control_settings_.begin(), runtime_scale_control_settings_.end(),
          [&](const RuntimeScaleControlSetting& retained) {
            return retained.resolution.same_control_scope(target);
          });
      if (same_scope != runtime_scale_control_settings_.end()) {
        if (target.members.size() == 1U &&
            same_scope->generation.value() == std::numeric_limits<std::uint64_t>::max()) {
          return reject<RuntimeScaleControlAcceptedDTO>(
              "runtime-scale member generation space is exhausted",
              "runtime_scale_control_generation_exhausted", ErrorCode::kInternalError);
        }
        continue;
      }
      const auto same_selector = std::find_if(
          runtime_scale_control_settings_.begin(), runtime_scale_control_settings_.end(),
          [&](const RuntimeScaleControlSetting& retained) {
            return retained.resolution.selector == target.selector;
          });
      if (same_selector == runtime_scale_control_settings_.end()) {
        ++projected_setting_count;
      } else {
        projected_memberships -= same_selector->resolution.members.size();
      }
      projected_memberships += target.members.size();
    }
    if (projected_setting_count > registrations_.size() + kMaxProgramExecutionGroups ||
        projected_memberships > kMaxProgramExecutionMemberships) {
      return reject<RuntimeScaleControlAcceptedDTO>(
          "runtime-scale control setting capacity is exhausted",
          "runtime_scale_control_capacity_exhausted", ErrorCode::kInvalidStateTransition);
    }
  }

  const auto projected = get_runtime_scale_control(request.selector);
  if (!projected.ok()) {
    return Result<RuntimeScaleControlAcceptedDTO>::Err(projected.error());
  }
  if (!projected.value().supported || projected.value().frozen) {
    return Result<RuntimeScaleControlAcceptedDTO>::Err(projected.value().reason.value_or(error(
        std::string(to_string(ErrorCode::kInvalidStateTransition)),
        "runtime-scale control is not currently available", "runtime_scale_control_unavailable")));
  }

  RuntimeScaleControlAcceptedDTO accepted{
      request.request_context.request_id,
      RequestReplayStatusDTO::kAcceptedNew,
      request.selector,
      request.scale,
      RuntimeScaleControlGeneration(0U),  // the next generation, below
      std::nullopt,
      std::nullopt,
      std::nullopt,
      clock_.now()};
  std::vector<RuntimeScaleRequestDTO> live_requests;
  // OD-72: every participant of the scope gets the value as its ramp target,
  // idle or moving (a starting motion reads it when it registers its ramp).
  std::vector<IResolvedParticipantExecutionEngine*> setting_targets;
  std::uint64_t value_sequence = 0U;
  {
    std::lock_guard state_lock(state_mutex_);
    const auto resolved = resolve_runtime_scale_control_locked(request.selector);
    if (!resolved.ok()) {
      return Result<RuntimeScaleControlAcceptedDTO>::Err(resolved.error());
    }
    auto setting =
        std::find_if(runtime_scale_control_settings_.begin(), runtime_scale_control_settings_.end(),
                     [&](const RuntimeScaleControlSetting& candidate) {
                       return candidate.resolution.same_control_scope(resolved.value());
                     });
    const auto current_generation = setting == runtime_scale_control_settings_.end()
                                        ? RuntimeScaleControlGeneration(0U)
                                        : setting->generation;
    if (current_generation.value() == std::numeric_limits<std::uint64_t>::max()) {
      return reject<RuntimeScaleControlAcceptedDTO>(
          "runtime-scale control generation space is exhausted",
          "runtime_scale_control_generation_exhausted", ErrorCode::kInternalError);
    }
    accepted.control_generation = RuntimeScaleControlGeneration(current_generation.value() + 1U);
    value_sequence = next_runtime_scale_value_sequence();
    const auto upsert_setting = [&](RuntimeScaleControlScope resolution,
                                    const RuntimeScaleControlGeneration generation) {
      auto retained = std::find_if(
          runtime_scale_control_settings_.begin(), runtime_scale_control_settings_.end(),
          [&](const RuntimeScaleControlSetting& candidate) {
            return candidate.resolution.same_control_scope(resolution);
          });
      if (retained == runtime_scale_control_settings_.end()) {
        // A portable selector replaces a stale route resolution instead of
        // accumulating aliases as control configurations change.
        std::erase_if(runtime_scale_control_settings_, [&](const auto& candidate) {
          return candidate.resolution.selector == resolution.selector;
        });
        runtime_scale_control_settings_.push_back(
            RuntimeScaleControlSetting{std::move(resolution), generation, request.scale});
      } else {
        retained->resolution = std::move(resolution);
        retained->generation = generation;
        retained->requested_scale = request.scale;
      }
    };
    upsert_setting(resolved.value(), accepted.control_generation);
    if (resolved.value().members.size() > 1U) {
      for (const auto& member : resolved.value().members) {
        RuntimeScaleControlScope singular{member.selector, {member}};
        const auto retained = std::find_if(
            runtime_scale_control_settings_.begin(), runtime_scale_control_settings_.end(),
            [&](const RuntimeScaleControlSetting& candidate) {
              return candidate.resolution.same_control_scope(singular);
            });
        const auto generation = retained == runtime_scale_control_settings_.end()
                                    ? RuntimeScaleControlGeneration(1U)
                                    : RuntimeScaleControlGeneration(retained->generation.value() + 1U);
        upsert_setting(std::move(singular), generation);
      }
    }
    const auto matches_scope = [&](const RuntimeParticipantBindingDTO& binding) {
      if (!binding.control_path || !binding.control_path->runtime_scale_qualification) return false;
      const auto& control = *binding.control_path;
      const auto& qualification = *control.runtime_scale_qualification;
      return std::any_of(
          resolved.value().members.begin(), resolved.value().members.end(),
          [&](const auto& member) {
            return control.control_domain_id == member.control_domain_id &&
                   control.control_domain_revision == member.control_domain_revision &&
                   control.control_binding_id == member.control_binding_id &&
                   control.control_binding_revision == member.control_binding_revision &&
                   qualification.scaling_domain_id == member.scaling_domain_id &&
                   control.active_control_configuration_id ==
                       member.active_control_configuration_id &&
                   control.active_control_configuration_generation ==
                       member.active_control_configuration_generation;
          });
    };
    // A coordinated operation's participants keep their shared scale
    // (coordinated runtime scaling stays refused); every other participant
    // of the scope takes the value.
    std::set<const IResolvedParticipantExecutionEngine*> coordinated_engines;
    for (const auto& [operation_id, operation] : active_operations_) {
      if (terminal(operation->state.state) || operation->participants.size() < 2U) continue;
      for (const auto& participant : operation->participants)
        coordinated_engines.insert(participant.engine);
    }
    for (const auto& registration : registrations_) {
      if (registration.engine == nullptr || !matches_scope(registration.binding.value()) ||
          coordinated_engines.contains(registration.engine) ||
          std::find(setting_targets.begin(), setting_targets.end(), registration.engine) !=
              setting_targets.end())
        continue;
      setting_targets.push_back(registration.engine);
    }
    // Lifetime borrow of these engines until the store calls below return.
    if (!setting_targets.empty()) ++runtime_scale_effect_calls_in_flight_;
    for (const auto& [operation_id, operation] : active_operations_) {
      if (terminal(operation->state.state) || !operation->runtime_scale_status) continue;
      const bool selected = std::any_of(
          operation->participants.begin(), operation->participants.end(),
          [&](const auto& participant) {
            return participant.payload_kind == OperationParticipantPayloadKind::kMotion &&
                   matches_scope(participant.binding.value());
          });
      if (!selected) continue;
      auto context = request.request_context;
      std::string member_request_material("manyforge.runtime-scale-group-member.v1");
      member_request_material.push_back('\0');
      member_request_material.append(request.request_context.request_id.value());
      member_request_material.push_back('\0');
      member_request_material.append(operation_id.value());
      context.request_id =
          RequestId(manyforge::content_identity::sha256_digest(member_request_material));
      live_requests.push_back(RuntimeScaleRequestDTO{
          std::move(context), operation_id, operation->state.operation_revision,
          operation->runtime_scale_status->runtime_scale_generation, request.scale,
          request.attribution});
    }
    std::sort(live_requests.begin(), live_requests.end(), [](const auto& left, const auto& right) {
      return left.operation_id < right.operation_id;
    });
    runtime_scale_control_replay_ledger_.erase_if(
        [&](const RuntimeScaleControlReplayEntry& entry) {
          return entry.accepted.selector == request.selector ||
                 entry.scope.same_control_scope(resolved.value());
        });
    // At most request_replay_capacity entries; the oldest leaves first, as in
    // the Stop ledger. A replay of an evicted id is a new request.
    if (runtime_scale_control_replay_ledger_.size() >= config_.request_replay_capacity)
      runtime_scale_control_replay_ledger_.pop_front();
    runtime_scale_control_replay_ledger_.push_back(
        RuntimeScaleControlReplayEntry{request, accepted, resolved.value()});
  }
  // The setting and its live targets are recorded: release submission
  // ordering before the journal writes, engine calls and SetScale publishes
  // below. Per-operation ordering stays with OperationCommitGuard and the
  // runtime-scale generation captured in each live request.
  submit_lock.unlock();

  if (!setting_targets.empty()) {
    const ScopeExit release_setting_borrow([&] {
      std::lock_guard state_lock(state_mutex_);
      --runtime_scale_effect_calls_in_flight_;
    });
    for (auto* engine : setting_targets)
      engine->store_runtime_scale_setting(request.scale, value_sequence);
  }

  // Independent group members keep their ordinary operation ownership. The
  // group command simply applies the accepted setting through each existing
  // exact operation-scale path. A concurrent Stop may make a member no longer
  // applicable; the retained setting is still authoritative.
  std::optional<RuntimeScaleAcceptedDTO> only_live_acceptance;
  for (auto& exact : live_requests) {
    auto applied = set_runtime_scale_impl(exact, value_sequence);
    if (applied.ok() && live_requests.size() == 1U)
      only_live_acceptance = std::move(applied.value());
  }
  if (only_live_acceptance) {
    accepted.active_operation_id = only_live_acceptance->operation_id;
    accepted.active_operation_revision = only_live_acceptance->operation_revision;
    accepted.active_operation_scale_generation = only_live_acceptance->runtime_scale_generation;
    std::lock_guard state_lock(state_mutex_);
    const auto replay =
        runtime_scale_control_replay_ledger_.find(request.request_context.request_id);
    if (replay != runtime_scale_control_replay_ledger_.end()) replay->accepted = accepted;
  }
  return Result<RuntimeScaleControlAcceptedDTO>::Ok(std::move(accepted));
}

Result<void> ResourceAddressedExecutionKernel::retain_runtime_scale_observation_locked(
    std::unique_lock<std::mutex>& state_lock, ActiveOperation& operation,
    RuntimeScaleStatusDTO status, const char* reason) {
  const auto& prior = *operation.runtime_scale_status;
  // Freshness and virtual time are live observations, not journal events.
  // Keep every current sample but persist only a change of scale/application
  // or fault. Exact identity/generation checks precede this owner-local call.
  const bool unchanged =
      prior.operation_id == status.operation_id &&
      prior.operation_revision == status.operation_revision &&
      prior.runtime_scale_generation == status.runtime_scale_generation &&
      prior.requested_scale == status.requested_scale &&
      prior.effective_scale == status.effective_scale && prior.state == status.state &&
      std::equal(prior.participants.begin(), prior.participants.end(), status.participants.begin(),
                 status.participants.end(), [](const auto& left, const auto& right) {
                   return left.accepted_generation == right.accepted_generation &&
                          left.commanded_scale == right.commanded_scale &&
                          left.applied_scale == right.applied_scale &&
                          left.supported == right.supported && left.state == right.state &&
                          left.error == right.error;
                 });
  if (!unchanged) {
    auto captured = operation.state;
    auto durable = record_operation_state_locked(state_lock, captured, &status, reason);
    if (!durable.ok()) {
      return Result<void>::Err(durable.error());
    }
    if (!durable.value()) return Result<void>::Ok();
    operation.state = std::move(captured);
  }
  operation.runtime_scale_status = std::move(status);
  return Result<void>::Ok();
}

Result<void> ResourceAddressedExecutionKernel::apply_runtime_scale_status(
    IResolvedParticipantExecutionEngine* engine,
    const ResolvedRuntimeScaleStatusDTO& resolved_status) {
  OperationCommitGuard commit(*this, resolved_status.operation_id);
  std::unique_lock state_lock(state_mutex_);
  auto* active_operation = find_active_operation_locked(resolved_status.operation_id);
  if (active_operation == nullptr || active_operation->recovery_effect_in_flight ||
      !active_operation->runtime_scale_status.has_value()) {
    return Result<void>::Ok();
  }
  const auto* active_participant = active_operation->participant_for(engine);
  if (active_participant == nullptr ||
      resolved_status.operation_revision < active_operation->state.operation_revision) {
    return Result<void>::Ok();
  }
  auto status = *active_operation->runtime_scale_status;
  const auto row =
      std::find_if(status.participants.begin(), status.participants.end(), [&](const auto& item) {
        return item.participant_id == active_participant->slot_owner.participant_id;
      });
  if (row == status.participants.end()) return Result<void>::Ok();
  auto& participant = *row;
  const auto& binding = active_participant->binding.value();
  const auto& control = *binding.control_path;
  const auto& qualification = *control.runtime_scale_qualification;
  const bool exact_target_matches =
      resolved_status.operation_id == status.operation_id &&
      resolved_status.operation_revision == status.operation_revision &&
      resolved_status.participant_id == participant.participant_id &&
      resolved_status.controllable_component_id == participant.controllable_component_id &&
      resolved_status.command_target_id == participant.command_target_id &&
      resolved_status.capability_id == participant.capability_id &&
      resolved_status.control_domain_id == participant.control_domain_id &&
      resolved_status.control_domain_revision == participant.control_domain_revision &&
      resolved_status.control_binding_id == participant.control_binding_id &&
      resolved_status.control_binding_revision == participant.control_binding_revision &&
      resolved_status.controller_goal_id == participant.controller_goal_id &&
      resolved_status.controller_goal_generation == participant.controller_goal_generation &&
      resolved_status.controller_topology_id == participant.controller_topology_id &&
      resolved_status.topology_revision == participant.topology_revision &&
      resolved_status.control_route_id == participant.control_route_id &&
      resolved_status.scaling_domain_id == participant.scaling_domain_id &&
      resolved_status.coordinated_barrier_id == participant.coordinated_barrier_id &&
      resolved_status.resources == participant.resources &&
      resolved_status.active_control_configuration_id ==
          participant.active_control_configuration_id &&
      resolved_status.active_control_configuration_generation ==
          participant.active_control_configuration_generation &&
      resolved_status.acknowledgement_source_epoch == control.provider_runtime_epoch;
  if (!exact_target_matches) {
    participant.state = RuntimeScaleApplicationState::kFailed;
    status.state = RuntimeScaleApplicationState::kFailed;
    participant.error =
        error(std::string(to_string(ErrorCode::kInvariantViolation)),
              "participant engine returned runtime-scale status for another binding",
              "runtime_scale_status_identity_mismatch");
    status.updated_at_ns = clock_.now();
    return retain_runtime_scale_observation_locked(state_lock, *active_operation, std::move(status),
                                                   "runtime_scale_status_identity_mismatch");
  }
  if (resolved_status.accepted_generation < status.runtime_scale_generation) {
    return Result<void>::Ok();
  }
  if (resolved_status.accepted_generation != status.runtime_scale_generation) {
    participant.state = RuntimeScaleApplicationState::kFailed;
    status.state = RuntimeScaleApplicationState::kFailed;
    participant.error = error(std::string(to_string(ErrorCode::kInvariantViolation)),
                              "participant engine advanced an unrequested runtime-scale generation",
                              "runtime_scale_status_generation_mismatch");
    status.updated_at_ns = clock_.now();
    return retain_runtime_scale_observation_locked(state_lock, *active_operation, std::move(status),
                                                   "runtime_scale_status_generation_mismatch");
  }
  const auto valid_scale = [](const double scale) {
    return std::isfinite(scale) && scale >= 0.0 && scale <= 1.0;
  };
  const auto observed_state = resolved_status.state;
  if (resolved_status.acknowledgement_timestamp_ns < 0 ||
      resolved_status.acknowledgement_sequence > 9'007'199'254'740'991ULL ||
      !std::isfinite(resolved_status.virtual_trajectory_time_s) ||
      resolved_status.virtual_trajectory_time_s < 0.0 ||
      !valid_scale(resolved_status.commanded_scale) ||
      !valid_scale(resolved_status.applied_scale) ||
      (observed_state != RuntimeScaleApplicationState::kPending &&
       observed_state != RuntimeScaleApplicationState::kApplied &&
       observed_state != RuntimeScaleApplicationState::kRejected &&
       observed_state != RuntimeScaleApplicationState::kFailed)) {
    // Unchanged observations skip serialization, not validation. Never put
    // invalid provider scalars into the live owner projection.
    participant.state = RuntimeScaleApplicationState::kFailed;
    status.state = RuntimeScaleApplicationState::kFailed;
    participant.error = error(std::string(to_string(ErrorCode::kInvariantViolation)),
                              "participant engine returned invalid runtime-scale values",
                              "runtime_scale_status_invalid");
    status.updated_at_ns = clock_.now();
    return retain_runtime_scale_observation_locked(state_lock, *active_operation, std::move(status),
                                                   "runtime_scale_status_invalid");
  }
  participant.commanded_scale = resolved_status.commanded_scale;
  participant.applied_scale = resolved_status.applied_scale;
  participant.virtual_trajectory_time_s = resolved_status.virtual_trajectory_time_s;
  participant.acknowledgement_source_epoch = resolved_status.acknowledgement_source_epoch;
  participant.acknowledgement_sequence = resolved_status.acknowledgement_sequence;
  participant.acknowledgement_timestamp_ns = resolved_status.acknowledgement_timestamp_ns;
  participant.supported = resolved_status.state != RuntimeScaleApplicationState::kRejected;
  participant.error = resolved_status.error;
  status.updated_at_ns = clock_.now();

  const bool stale = resolved_status.acknowledgement_timestamp_ns > 0 &&
                     status.updated_at_ns > resolved_status.acknowledgement_timestamp_ns &&
                     status.updated_at_ns - resolved_status.acknowledgement_timestamp_ns >
                         qualification.freshness_budget_ns;
  if (resolved_status.error.has_value() || stale) {
    participant.state = RuntimeScaleApplicationState::kFailed;
    status.state = RuntimeScaleApplicationState::kFailed;
    if (stale && !participant.error.has_value()) {
      participant.error = error(std::string(to_string(ErrorCode::kExecutionRejected)),
                                "runtime-scale applied-state acknowledgement is stale",
                                "runtime_scale_acknowledgement_stale");
    }
  } else {
    participant.state = resolved_status.state;
  }
  status.effective_scale = std::min_element(status.participants.begin(), status.participants.end(),
                                            [](const auto& left, const auto& right) {
                                              return left.applied_scale < right.applied_scale;
                                            })
                               ->applied_scale;
  status.state = RuntimeScaleApplicationState::kApplied;
  for (const auto& item : status.participants) {
    if (item.state == RuntimeScaleApplicationState::kFailed ||
        item.state == RuntimeScaleApplicationState::kRejected) {
      status.state = item.state;
      break;
    }
    if (item.state != RuntimeScaleApplicationState::kApplied ||
        std::abs(item.commanded_scale - status.requested_scale) > 1e-12 ||
        std::abs(item.applied_scale - status.requested_scale) > 1e-12)
      status.state = RuntimeScaleApplicationState::kPending;
  }
  return retain_runtime_scale_observation_locked(state_lock, *active_operation, std::move(status),
                                                 "runtime_scale_status_observed");
}

Result<RuntimeScaleStatusDTO> ResourceAddressedExecutionKernel::get_runtime_scale(
    const OperationId& operation_id) const {
  if (operation_id.empty()) {
    return reject<RuntimeScaleStatusDTO>("operation identity is empty", "operation_id_empty",
                                         ErrorCode::kInvalidRequest);
  }
  std::lock_guard state_lock(state_mutex_);
  const auto* active_operation = find_active_operation_locked(operation_id);
  if (active_operation == nullptr) {
    return reject<RuntimeScaleStatusDTO>("operation is not resident", "operation_not_resident",
                                         ErrorCode::kMissingReference);
  }
  if (!active_operation->runtime_scale_status.has_value()) {
    return reject<RuntimeScaleStatusDTO>("operation binding has no qualified runtime scaling",
                                         "runtime_scale_unsupported",
                                         ErrorCode::kUnsupportedCapability);
  }
  return Result<RuntimeScaleStatusDTO>::Ok(*active_operation->runtime_scale_status);
}

}  // namespace manyforge::core
