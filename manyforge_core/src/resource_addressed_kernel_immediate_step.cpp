#include "resource_addressed_execution_kernel_internal.hpp"

namespace manyforge::core {

// A2a-2: an immediate component step is admitted like any operation of its
// component, then the adapter's atomic step runs synchronously and outside
// the Kernel locks. It is never resident, so no Stop, reset or recovery path
// reaches it, and nothing is journaled or acknowledged.
Result<OperationAcceptedDTO> ResourceAddressedExecutionKernel::execute_immediate_step(
    const OperationIntentDTO& intent,
    const std::function<Result<ImmediateComponentStep>(
        const RuntimeParticipantExecutionRegistration&)>& step_for) {
  std::optional<Result<RuntimeParticipantExecutionRegistration>> resolved;
  std::optional<ImmediateComponentStep> step;
  std::optional<OperationAcceptedDTO> accepted;
  bool holding = false;
  {
    std::lock_guard submit_lock(submission_mutex_);
    if (const auto valid = validate_operation_intent(intent); !valid.ok()) {
      return Result<OperationAcceptedDTO>::Err(valid.error());
    }
    if (intent.request_context.request_owner_epoch != config_.request_owner_epoch) {
      return Result<OperationAcceptedDTO>::Err(
          error("stale_request_owner_epoch", "operation request names a stale Kernel owner epoch",
                "stale_request_owner_epoch"));
    }
    if (intent.participants.size() != 1U ||
        intent.coordination_policy != OperationCoordinationPolicy::kIndependent) {
      return reject<OperationAcceptedDTO>("an immediate step names exactly one component",
                                          "operation_coordination_shape_invalid");
    }
    resolved.emplace(resolve_registration(intent, 0U));
    if (!resolved->ok()) return Result<OperationAcceptedDTO>::Err(resolved->error());
    const auto& registration = resolved->value();
    auto made = step_for(registration);
    if (!made.ok()) return Result<OperationAcceptedDTO>::Err(made.error());
    step.emplace(std::move(made.value()));
    const bool manual = registration.intrinsic_direct_control;
    if (manual ? intent.participants.front().execution_scope.has_value()
               : intent.manual_handover.has_value()) {
      return reject<OperationAcceptedDTO>(
          "manual work carries no Program-node authority, and only manual work names a hold",
          "direct_control_product_mismatch");
    }
    std::optional<ResolvedManualProgramHandover> handover;
    if (intent.manual_handover) {
      auto held = resolve_manual_program_handover(registration);
      if (!held.ok()) return Result<OperationAcceptedDTO>::Err(held.error());
      if (!held.value()) {
        return reject<OperationAcceptedDTO>("manual handover has no current Program",
                                            "manual_handover_scope_stale");
      }
      const auto& capture = held.value()->capture;
      const ManualProgramHandoverContextDTO current{capture.program_runner_owner_epoch,
                                                    capture.program_run_id,
                                                    capture.claim_generation,
                                                    held.value()->selection_revision};
      if (current != *intent.manual_handover) {
        return reject<OperationAcceptedDTO>("manual handover differs from its current hold",
                                            "manual_handover_scope_stale");
      }
      handover = std::move(held.value());
    }
    std::lock_guard admission_lock(*admission_mutex_);
    std::unique_lock state_lock(state_mutex_, std::defer_lock);
    if (manual) {
      // T-A2b: Robot control's own manual work (a jog) already holds this
      // component, so its origin is proven; the busy rule below then decides
      // (a step that does not hold the engine runs during the jog).
      const auto& component = registration.binding.value().resources;
      state_lock.lock();
      const bool held_by_manual_work =
          manual_operation_claim_ &&
          std::all_of(component.begin(), component.end(), [&](const auto& resource) {
            const auto& held = manual_operation_claim_->intent.scope.resources;
            return std::binary_search(held.begin(), held.end(), resource.resource_id);
          });
      state_lock.unlock();
      // Otherwise the origin of manual work, as for a manual move; the step
      // holds no claim of its own (it is never resident).
      if (!held_by_manual_work) {
        const auto ready = cell_admission_owner_->CheckManualCellAdmissionReadiness(
            handover ? std::optional(handover->capture.claim_generation) : std::nullopt);
        if (!ready.ok()) return Result<OperationAcceptedDTO>::Err(ready.error());
      }
    }
    state_lock.lock();
    // A2a-1 (decided): no "component enabled" or "motion admission open"
    // check; the adapter's step handles a component that is not enabled.
    // A non-motion transaction, as an output: its node's own stop policy
    // decides whether it is sent after a Program Stop (T-MQ15).
    if (const auto scope = validate_program_execution_scope_locked(intent, true, manual);
        !scope.ok()) {
      return Result<OperationAcceptedDTO>::Err(scope.error());
    }
    if (handover) {
      if (const auto held = validate_manual_program_handover_locked(*handover); !held.ok()) {
        return Result<OperationAcceptedDTO>::Err(held.error());
      }
    }
    if (step->holds_engine) {
      auto* engine = registration.engine;
      if (immediate_step_engines_.contains(engine) ||
          std::any_of(active_operations_.begin(), active_operations_.end(), [&](const auto& item) {
            return item.second->participant_for(engine) != nullptr;
          })) {
        return reject<OperationAcceptedDTO>("the component is busy with another operation",
                                            "participant_engine_busy");
      }
      immediate_step_engines_.insert(engine);
      holding = true;
    }
    const auto operation_id = OperationId("operation." + config_.request_owner_epoch.value() +
                                          "." + std::to_string(next_operation_id_++));
    accepted.emplace(OperationAcceptedDTO{
        intent.request_context.request_id,
        RequestReplayStatusDTO::kAcceptedNew,
        operation_id,
        OperationRevision(1U),
        {OperationParticipantAssignmentDTO{
            0U, OperationParticipantId(operation_id.value() + ".participant.1")}},
        clock_.now()});
  }
  auto* engine = resolved->value().engine;
  const ScopeExit release_engine([&] {
    if (!holding) return;
    std::lock_guard state_lock(state_mutex_);
    immediate_step_engines_.erase(engine);
  });
  const auto ran = step->run();
  if (!ran.ok()) {
    // A2b: a failure after admission carries the accepted receipt, so the
    // caller tells it from a refusal before admission (nothing ran). The
    // step's own reason stays; nothing is resident or replayed.
    auto failed = ran.error();
    failed.details.insert_or_assign("immediate_step", "admitted");
    failed.details.insert_or_assign("request_id", intent.request_context.request_id.value());
    failed.details.insert_or_assign("operation_id", accepted->operation_id.value());
    return Result<OperationAcceptedDTO>::Err(std::move(failed));
  }
  return Result<OperationAcceptedDTO>::Ok(std::move(*accepted));
}

// Active TCP (M7, T-MQ15, T-A2): Set TCP is the first immediate step. The
// checkbox decides whether it holds the robot.
Result<OperationAcceptedDTO> ResourceAddressedExecutionKernel::execute_set_tcp(
    const OperationIntentDTO& intent) {
  return execute_immediate_step(
      intent, [&](const RuntimeParticipantExecutionRegistration& registration)
                  -> Result<ImmediateComponentStep> {
        if (!registration.set_tcp) {
          return reject<ImmediateComponentStep>("this robot target cannot set its Active TCP",
                                                "set_tcp_unavailable",
                                                ErrorCode::kUnsupportedCapability);
        }
        const auto& binding = registration.binding.value();
        const auto owner =
            registration.motion_endpoint_owner.value_or(binding.controllable_component_id);
        const auto& payload =
            std::get<SetTcpParticipantIntentDTO>(intent.participants.front().payload);
        // The registry checked target and frame; the owner is the registration's.
        if (!tcp_selector_admits(payload.tcp, *binding.motion_endpoint, owner)) {
          return reject<ImmediateComponentStep>("Set TCP lacks the robot's exact runtime mapping",
                                                "runtime_motion_mapping_incomplete");
        }
        return Result<ImmediateComponentStep>::Ok(ImmediateComponentStep{
            registration.set_tcp->holds_robot,
            [apply = registration.set_tcp->apply,
             entry = tcp_entry_of(*binding.motion_endpoint, owner),
             robot_control = registration.intrinsic_direct_control] {
              return apply(entry, robot_control);
            }});
      });
}

}  // namespace manyforge::core
