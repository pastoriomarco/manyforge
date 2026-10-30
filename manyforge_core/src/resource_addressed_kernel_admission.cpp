#include "resource_addressed_execution_kernel_internal.hpp"
#include "admission_timing_internal.hpp"
#include <manyforge_core/io_signal_descriptor.hpp>
#include <manyforge_core/io_condition_evaluator.hpp>
#include <manyforge_core/types/program_io_condition.hpp>
#include "component_lifecycle_journal_codec_internal.hpp"

#include <cstdio>

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

void ResourceAddressedExecutionKernel::mark_motion_retry_run_end_locked(
    const ProgramRunId& run_id) {
  retire_program_io_watches_locked(run_id);
  for (auto& [id, operation] : active_operations_) {
    static_cast<void>(id);
    if (operation->state.program_run_id == run_id &&
        fresh_motion_node_indices_locked(*operation))
      operation->retry_run_ended = true;
  }
}

Result<AuthoringAdmissionDTO> ResourceAddressedExecutionKernel::UpdateCellAdmissionReadiness(
    const CellAdmissionReadinessSnapshotDTO& snapshot,
    std::shared_ptr<IIoSignalService>* actual_retired_service) {
  if (actual_retired_service) actual_retired_service->reset();  // Destroy outside Core locks.
  auto updated = [&] {
    std::lock_guard admission_lock(*admission_mutex_);
    if (snapshot.program_runner_loss) {
      const auto valid = cell_admission_owner_->ValidateProgramRunnerContactLossSource(
          *snapshot.program_runner_loss);
      if (!valid.ok()) return Result<AuthoringAdmissionDTO>::Err(valid.error());
      if (snapshot.program_cycle_status_current)
        return reject<AuthoringAdmissionDTO>("Runner loss must close currentness",
                                             "program_runner_contact_loss_invalid",
                                             ErrorCode::kInvalidRequest);
      std::lock_guard state_lock(state_mutex_);
      const auto& cycle = snapshot.program_runner_loss->established_cycle;
      const bool exact_replay = program_runner_loss_ == snapshot.program_runner_loss;
      if (!exact_replay &&
          (!program_cycle_claim_run_ ||
           program_cycle_claim_run_->program_runner_owner_epoch !=
               cycle.program_runner_owner_epoch ||
           program_cycle_claim_run_->program_run_id != cycle.program_run_id ||
           program_cycle_claim_run_->claim_generation != cycle.cycle_claim_generation)) {
        return reject<AuthoringAdmissionDTO>("Runner loss does not name the held Program owner",
                                             "program_runner_contact_loss_stale",
                                             ErrorCode::kInvalidRequest);
      }
      // The first owner-issued fault remains observable even if advancing
      // ordinary readiness fails its journal write. This grants no recovery.
      if (!program_runner_loss_) program_runner_loss_ = snapshot.program_runner_loss;
      const auto bb_retired = retire_program_blackboard_source_locked(actual_retired_service);
      if (!bb_retired.ok()) return Result<AuthoringAdmissionDTO>::Err(bb_retired.error());
      if (cycle.program_run_id) retire_program_io_watches_locked(*cycle.program_run_id);
    }
    auto result = cell_admission_owner_->UpdateCellAdmissionReadiness(snapshot);
    if (result.ok()) {
      std::lock_guard state_lock(state_mutex_);
      const auto bb_current = reconcile_program_blackboard_cycle_locked(
          snapshot.program_cycle_status, actual_retired_service);
      if (!bb_current.ok()) return Result<AuthoringAdmissionDTO>::Err(bb_current.error());
      current_program_run_id_ =
          snapshot.program_cycle_status_current && snapshot.program_cycle_status
              ? snapshot.program_cycle_status->program_run_id
              : std::nullopt;
      if (program_cycle_claim_run_ && snapshot.program_cycle_status) {
        const auto& run = *program_cycle_claim_run_;
        const auto& status = *snapshot.program_cycle_status;
        const bool exact = status.program_runner_owner_epoch == run.program_runner_owner_epoch &&
                           status.program_run_id == run.program_run_id &&
                           status.cycle_claim_generation == run.claim_generation;
        if (snapshot.program_cycle_status_current && exact &&
            (status.state == ProgramCycleStateDTO::kTerminalAwaitingReset ||
             status.state == ProgramCycleStateDTO::kResetting))
          mark_motion_retry_run_end_locked(run.program_run_id);
      }
    }
    return result;
  }();
  return updated;
}

std::optional<ProgramRunnerContactLossDTO>
ResourceAddressedExecutionKernel::get_program_runner_loss() const {
  std::lock_guard state_lock(state_mutex_);
  return program_runner_loss_;
}

Result<CellAdmissionClaimDTO> ResourceAddressedExecutionKernel::AcquireCellAdmissionClaim(
    const CellAdmissionClaimIntentDTO& intent) {
  return acquire_cell_admission_claim(intent, false);
}

namespace {

/// The claim names exactly this ordinary CCLC lifecycle request.
[[nodiscard]] bool lifecycle_claim_binds(const CellAdmissionClaimIntentDTO& claim,
                                         const ComponentLifecycleIntentDTO& lifecycle) {
  return lifecycle.locally_valid() && !lifecycle.program_node_evidence &&
         claim.claim_kind == CellAdmissionClaimKindDTO::kComponentLifecycle &&
         claim.owner_role == CellAdmissionOwnerRoleDTO::kComponentLifecycleOwner &&
         claim.request_id == lifecycle.request_context.request_id &&
         claim.owner_epoch == lifecycle.request_context.request_owner_epoch &&
         claim.expected_admission_generation == lifecycle.expected_admission_generation &&
         claim.request_payload_digest ==
             ContentDigest(detail::ComponentLifecycleSemanticIntentDigest(lifecycle));
}

}  // namespace

Result<CellAdmissionClaimDTO> ResourceAddressedExecutionKernel::AcquireComponentLifecycleClaim(
    const CellAdmissionClaimIntentDTO& claim, const ComponentLifecycleIntentDTO& lifecycle) {
  // D6: only a claim that is exactly a Disable may be one of Close's steps.
  return acquire_cell_admission_claim(
      claim, lifecycle.intent == ComponentLifecycleIntentKindDTO::kDisable &&
                 lifecycle_claim_binds(claim, lifecycle));
}

Result<CellAdmissionClaimDTO> ResourceAddressedExecutionKernel::acquire_cell_admission_claim(
    const CellAdmissionClaimIntentDTO& intent, const bool close_disable) {
  detail::AdmissionTiming timing(false, intent.request_id.value());
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
  std::optional<KernelCellAdmissionOwner::PreparedProgramResetStatus> reset_proof;
  if (intent.claim_kind == CellAdmissionClaimKindDTO::kProgramReset) {
    auto prepared = cell_admission_owner_->PrepareProgramResetStatus(
        ProgramResetJournalReadPurpose::kAcquire, intent);
    if (!prepared.ok()) return Result<CellAdmissionClaimDTO>::Err(prepared.error());
    reset_proof.emplace(std::move(prepared).value());
    // A preempted Runner task has already ended. Join its existing motion-free
    // cleanup before the whole-cell work check, without holding admission or
    // submission locks over participant calls. Actual in-flight work still
    // refuses this claim and remains available for a later explicit retry.
    const auto retired = retire_preempted_motion_operations();
    if (!retired.ok()) return Result<CellAdmissionClaimDTO>::Err(retired.error());
  }
  // Lifecycle and selected idle Reset mutate the whole cell. Serialize the
  // physical work check with every command submission, then retain the
  // admission lock through the durable claim so Start cannot win in between.
  std::unique_lock<std::mutex> submit_lock(submission_mutex_, std::defer_lock);
  const bool physical_mutation = intent.claim_kind == CellAdmissionClaimKindDTO::kComponentLifecycle ||
                                 intent.claim_kind == CellAdmissionClaimKindDTO::kProgramReset;
  if (physical_mutation) {
    const auto wait_start = detail::AdmissionTiming::Clock::now();
    submit_lock.lock();
    timing.record->submission_wait += detail::AdmissionTiming::Clock::now() - wait_start;
  }
  const auto admission_wait_start = detail::AdmissionTiming::Clock::now();
  std::unique_lock admission_lock(*admission_mutex_);
  timing.record->admission_wait += detail::AdmissionTiming::Clock::now() - admission_wait_start;
  if (physical_mutation) {
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
          intent.claim_kind == CellAdmissionClaimKindDTO::kProgramReset
              ? "Program Reset requires settled physical and Program work"
              : "lifecycle mutation requires settled physical and Program work",
          intent.claim_kind == CellAdmissionClaimKindDTO::kProgramReset
              ? "program_reset_effects_not_settled"
              : blocking.size() == 1U && !busy_resources.empty()
              ? "component_lifecycle_resources_not_settled"
              : "component_lifecycle_effects_not_settled");
      auto error = rejected.error();
      error.details["blocking_conditions"] = join(blocking);
      if (!operations.empty()) error.details["active_operation_ids"] = join(operations);
      if (!busy_resources.empty()) error.details["busy_resource_ids"] = join(busy_resources);
      return Result<CellAdmissionClaimDTO>::Err(std::move(error));
    }
  }
  auto acquired = reset_proof
      ? cell_admission_owner_->AcquirePreparedProgramResetClaim(intent, *reset_proof)
      : close_disable ? cell_admission_owner_->AcquireCloseDisableLifecycleClaim(intent)
                      : cell_admission_owner_->AcquireCellAdmissionClaim(intent);
  if (acquired.ok() && intent.claim_kind == CellAdmissionClaimKindDTO::kProgramCycle) {
    const auto refresh_start = detail::AdmissionTiming::Clock::now();
    refresh_execution_claims();
    timing.record->refresh += detail::AdmissionTiming::Clock::now() - refresh_start;
  }
  admission_lock.unlock();
  return acquired;
}

Result<const ResourceAddressedExecutionKernel::ActiveOperation*>
ResourceAddressedExecutionKernel::qualify_io_output_recovery_operation_locked(
    const ControllableComponentId& component) const {
  using Reply = Result<const ActiveOperation*>;
  const auto refuse = [] {
    return Reply::Err(
        error("invalid_state_transition",
              "Local output resynchronization requires only its exact retained unknown owner",
              "io_output_recovery_owner_not_exact"));
  };
  if (active_operations_.size() != 1U || pending_program_run_admission_ ||
      operation_control_reconfiguration_busy_locked() || motion_admission_unreconciled_ ||
      motion_admission_closed_ || manager_liveness_motion_inhibited_ ||
      operation_journal_write_fault_ || is_active_stop_fault_latch(active_stop_fault_latch_) ||
      (!config_.operation_admission_gate ||
       config_.operation_admission_gate->has_held_configuration_fences()) ||
      std::any_of(stop_transactions_.begin(), stop_transactions_.end(),
                  [](const auto& stop) { return stop->active && !stop->terminal; }))
    return refuse();
  const auto& operation = *active_operations_.begin()->second;
  if (operation.state.state != OperationStateKind::kInterventionRequired ||
      operation.participants.empty() ||
      operation.participants.size() != operation.state.participants.size() ||
      operation.pending_control || operation.ordinary_acceptance_pending ||
      operation.ordinary_commit_in_flight || operation.runtime_scale_effect_in_flight ||
      operation.stop_effect_in_flight || operation.recovery_effect_in_flight ||
      operation.coordinated_cancellation_in_flight || operation.takeover_advance_in_flight ||
      !operation.pending_motion_takeover.empty() || !operation.next_motion_takeover.empty())
    return refuse();
  for (std::size_t index = 0; index < operation.participants.size(); ++index) {
    const auto& participant = operation.participants[index];
    const auto& binding = participant.binding.value();
    const auto& state = operation.state.participants[index];
    if (binding.controllable_component_id != component || !binding.io_output_binding ||
        binding.control_path || !binding.resources.empty() || participant.stop_readiness ||
        participant.stop_execution_port ||
        participant.payload_kind != OperationParticipantPayloadKind::kCapabilityCommand ||
        state.state != OperationStateKind::kInterventionRequired ||
        state.output_evidence != IoOutputEvidence::kAcceptanceUnknown || !state.terminal_fact ||
        state.terminal_fact->effect != ParticipantEffectKnowledge::kEffectUncertain ||
        participant.dispatch_effect_in_flight || participant.prepare_in_flight ||
        participant.prepared_advance_in_flight || participant.reconciliation_in_flight ||
        participant.deferred_cancel_settlement || participant.deferred_stop_settlement ||
        participant.ownership_transferred)
      return refuse();
    const auto& output = *binding.io_output_binding;
    const auto current =
        runtime_route_configuration_readbacks_.find(output.provider.control_domain_id);
    if (current == runtime_route_configuration_readbacks_.end() || !current->second.provider())
      return refuse();
    const auto& actual = *current->second.provider();
    if (actual.provider_occurrence != output.provider ||
        actual.domain.provider()->provider.controllable_component_id != component ||
        actual.domain.provider()->provider.signal_configuration_digest !=
            output.signal_configuration_digest ||
        actual.retained_bundle_id != binding.retained_bundle_id ||
        actual.graph_revision != binding.graph_revision ||
        std::find(actual.domain.resource_ids().begin(), actual.domain.resource_ids().end(),
                  *output.signal.output_resource) == actual.domain.resource_ids().end())
      return refuse();
  }
  // The device must be nonmoving in the actual installed inventory, including
  // unused registrations. Arm-hosted GPIO never gets this generic exception.
  if (std::any_of(registrations_.begin(), registrations_.end(), [&](const auto& registration) {
        const auto& binding = registration.binding.value();
        return binding.controllable_component_id == component &&
               (!binding.io_output_binding || binding.control_path || !binding.resources.empty() ||
                registration.stop_readiness || registration.stop_execution_port);
      }))
    return refuse();
  for (const auto& slot : resource_slots_->snapshot()) {
    if (slot.state == ResourceSlotState::kFree && !slot.owner) continue;
    const auto participant = std::find_if(
        operation.participants.begin(), operation.participants.end(), [&](const auto& row) {
          return slot.owner && *slot.owner == row.slot_owner &&
                 std::find(row.resources.begin(), row.resources.end(), slot.resource_id) !=
                     row.resources.end();
        });
    if (participant == operation.participants.end() || slot.state != ResourceSlotState::kTerminal)
      return refuse();
  }
  return Reply::Ok(&operation);
}

Result<void> ResourceAddressedExecutionKernel::validate_io_output_recovery_claims_locked(
    const ControllableComponentId& component, std::span<const CellAdmissionClaimDTO> held,
    const CellAdmissionReadinessSnapshotDTO& readiness) const {
  const auto qualified = qualify_io_output_recovery_operation_locked(component);
  if (!qualified.ok()) return Result<void>::Err(qualified.error());
  const auto& operation = *qualified.value();
  const auto refuse = [] {
    return reject<void>("Local output resynchronization conflicts with another current owner",
                        "io_output_recovery_claim_conflict");
  };
  std::optional<CellAdmissionClaimGeneration> parent;
  const auto& parent_run = operation.manual && operation.manual_handover
                               ? operation.manual_handover->capture.program_run_id
                               : operation.state.program_run_id;
  if (operation.manual) {
    if (!manual_operation_claim_ || manual_operation_claim_->intent.owner_transaction_id !=
                                        operation.state.operation_id.value())
      return refuse();
    parent = parent_program_cycle_claim(manual_operation_claim_->intent);
  } else if (has_program_execution_scope(operation.state)) {
    if (!program_cycle_claim_run_ || !program_cycle_claim_run_->capture ||
        !program_cycle_claim_run_->capture->table ||
        program_cycle_claim_run_->program_run_id != operation.state.program_run_id)
      return refuse();
    parent = program_cycle_claim_run_->claim_generation;
  } else if (program_cycle_claim_run_) {
    // An unscoped request cannot borrow an unrelated live Run's authority.
    return refuse();
  }
  bool saw_manual = !operation.manual;
  bool saw_parent = !parent;
  for (const auto& claim : held) {
    if (operation.manual && claim == *manual_operation_claim_) {
      saw_manual = true;
      continue;
    }
    if (parent && claim.claim_generation == *parent &&
        claim.intent.claim_kind == CellAdmissionClaimKindDTO::kProgramCycle &&
        claim.intent.owner_transaction_id == parent_run.value() && readiness.program_cycle_status &&
        readiness.program_cycle_status->program_runner_owner_epoch == claim.intent.owner_epoch &&
        readiness.program_cycle_status->program_run_id == parent_run &&
        readiness.program_cycle_status->cycle_claim_generation == parent) {
      saw_parent = true;
      continue;
    }
    const auto& node = claim.intent.program_node_evidence;
    if (!node || !parent || node->parent_program_cycle_claim_generation != *parent ||
        node->program_run_id != parent_run || !program_cycle_claim_run_ ||
        node->program_runner_owner_epoch != program_cycle_claim_run_->program_runner_owner_epoch ||
        std::none_of(operation.state.participants.begin(), operation.state.participants.end(),
                     [&](const auto& row) {
                       return row.execution_scope &&
                              row.execution_scope->node_instance_id.value() ==
                                  node->node_instance_id &&
                              row.execution_scope->node_attempt_id == node->node_attempt_id;
                     }))
      return refuse();
  }
  if (!saw_manual || !saw_parent || !readiness.program_cycle_status) return refuse();
  const auto& cycle = *readiness.program_cycle_status;
  if (!parent) {
    if (cycle.state != ProgramCycleStateDTO::kIdleAfterReset) return refuse();
  } else {
    if (cycle.state == ProgramCycleStateDTO::kStarting ||
        cycle.state == ProgramCycleStateDTO::kStopping ||
        cycle.state == ProgramCycleStateDTO::kResetting ||
        cycle.state == ProgramCycleStateDTO::kIdleAfterReset)
      return refuse();
    if (cycle.state == ProgramCycleStateDTO::kRunning && operation.manual) {
      if (!operation.manual_handover || !operation.manual_handover->capture.table ||
          operation.manual_handover->capture.program_run_id != parent_run)
        return refuse();
      const auto registration =
          std::find_if(registrations_.begin(), registrations_.end(), [&](const auto& row) {
            const auto& binding = row.binding.value();
            const auto& captured = operation.participants.front().binding.value();
            return binding.controllable_component_id == captured.controllable_component_id &&
                   binding.command_target_id == captured.command_target_id &&
                   binding.io_output_binding.has_value();
          });
      if (registration == registrations_.end() ||
          !validate_manual_program_handover_locked(*operation.manual_handover).ok())
        return refuse();
    } else if (cycle.state == ProgramCycleStateDTO::kRunning) {
      const auto fence = std::find_if(
          program_run_stop_fences_.begin(), program_run_stop_fences_.end(),
          [&](const auto& row) { return row.program_run_id == operation.state.program_run_id; });
      if (!program_cycle_claim_run_ || !program_cycle_claim_run_->capture ||
          !program_cycle_claim_run_->capture->table || fence == program_run_stop_fences_.end())
        return refuse();
      const auto& nodes = program_cycle_claim_run_->capture->table->nodes;
      for (const auto& participant : operation.state.participants) {
        if (!participant.execution_scope) return refuse();
        const auto node = std::lower_bound(
            nodes.begin(), nodes.end(), participant.execution_scope->node_instance_id,
            [](const auto& row, const auto& id) { return row.instance_id < id; });
        if (node == nodes.end() ||
            node->instance_id != participant.execution_scope->node_instance_id ||
            fence->node_states.size() != nodes.size() ||
            program_node_effectively_running_locked(operation.state.program_run_id,
                                                    static_cast<std::size_t>(node - nodes.begin())))
          return refuse();
      }
    }
  }
  return Result<void>::Ok();
}

Result<CellAdmissionClaimDTO>
ResourceAddressedExecutionKernel::AcquireIoOutputRecoveryLifecycleClaim(
    const CellAdmissionClaimIntentDTO& claim, const ComponentLifecycleIntentDTO& lifecycle) {
  if (!lifecycle.locally_valid() ||
      (lifecycle.intent != ComponentLifecycleIntentKindDTO::kDisable &&
       lifecycle.intent != ComponentLifecycleIntentKindDTO::kPrepare &&
       lifecycle.intent != ComponentLifecycleIntentKindDTO::kEnable) ||
      lifecycle.program_node_evidence || lifecycle.apply_generation_selection ||
      lifecycle.close_incident_recovery ||
      claim.claim_kind != CellAdmissionClaimKindDTO::kComponentLifecycle ||
      claim.owner_role != CellAdmissionOwnerRoleDTO::kComponentLifecycleOwner ||
      claim.request_id != lifecycle.request_context.request_id ||
      claim.owner_epoch != lifecycle.request_context.request_owner_epoch ||
      claim.expected_admission_generation != lifecycle.expected_admission_generation ||
      claim.request_payload_digest !=
          ContentDigest(detail::ComponentLifecycleSemanticIntentDigest(lifecycle)))
    return reject<CellAdmissionClaimDTO>(
        "Local output recovery has no exact ordinary lifecycle request",
        "io_output_recovery_lifecycle_request_invalid", ErrorCode::kInvalidRequest);
  // Idle ordinary lifecycle keeps its exact original admission behavior.
  bool idle = false;
  {
    std::lock_guard state_lock(state_mutex_);
    idle = active_operations_.empty();
  }
  if (idle)
    return acquire_cell_admission_claim(
        claim, lifecycle.intent == ComponentLifecycleIntentKindDTO::kDisable);
  std::lock_guard submit_lock(submission_mutex_);
  std::lock_guard admission_lock(*admission_mutex_);
  return cell_admission_owner_->AcquireIoOutputRecoveryLifecycleClaim(
      claim, [&](const auto&, auto held, const auto& readiness, const auto&) -> Result<void> {
        std::lock_guard state_lock(state_mutex_);
        if (!stop_graph_revision_ || lifecycle.expected_graph_revision != *stop_graph_revision_)
          return reject<void>("Output recovery graph is not current",
                              "io_output_recovery_product_stale");
        const auto actual_count = std::count_if(
            runtime_route_configuration_readbacks_.begin(),
            runtime_route_configuration_readbacks_.end(), [&](const auto& row) {
              return row.second.provider() &&
                     row.second.provider()->domain.provider()->provider.controllable_component_id ==
                         lifecycle.controllable_component_id;
            });
        if (actual_count != static_cast<std::ptrdiff_t>(lifecycle.expected_control_domains.size()))
          return reject<void>("Output recovery domain coverage is incomplete",
                              "io_output_recovery_product_stale");
        for (const auto& expected : lifecycle.expected_control_domains) {
          const auto actual =
              runtime_route_configuration_readbacks_.find(expected.control_domain_id);
          if (!expected.provider() || actual == runtime_route_configuration_readbacks_.end() ||
              !actual->second.provider() ||
              actual->second.provider()->domain.provider()->provider.controllable_component_id !=
                  lifecycle.controllable_component_id ||
              !ExpectedDomainMatchesResolvedScope(expected, actual->second.provider()->domain))
            return reject<void>("Output recovery domain is not current",
                                "io_output_recovery_product_stale");
        }
        return validate_io_output_recovery_claims_locked(lifecycle.controllable_component_id, held,
                                                         readiness);
      });
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
            found == runtime_route_configuration_readbacks_.end() || found->second.provider() != nullptr ||
            found->second.controller().active_configuration != predecessor ||
            found->second.controller().operation_admission_open || !found->second.controller().readiness_rows.empty() ||
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
  return validate_bring_up_component_closure_locked(offer.predecessor, offer.candidate_participants,
                                                    offer.candidate_provider_domains,
                                                    offer.fence_admission);
}

Result<void> ResourceAddressedExecutionKernel::validate_bring_up_component_closure_locked(
    const ComponentLifecycleCurrentSnapshotDTO& predecessor,
    const std::vector<RuntimeParticipantBindingDTO>& candidates,
    const std::vector<ResolvedLifecycleControlDomainScopeDTO>& candidate_provider_domains,
    const ConfigurationFenceAdmissionSnapshotDTO& admission) {
  const auto& component = predecessor.controllable_component_id;
  std::vector<ResourceId> current_resources;
  std::set<ControlBindingId> current_bindings;
  for (const auto& [id, readback] : runtime_route_configuration_readbacks_) {
    const auto* current = readback.provider();
    if (current && current->domain.provider()->provider.controllable_component_id == component &&
        std::none_of(predecessor.domains.begin(), predecessor.domains.end(),
                     [&](const auto& domain) { return domain.control_domain_id == id; })) {
      return reject<void>("selected bring-up omits an installed predecessor provider domain",
                          "selected_bring_up_configuration_not_current");
    }
  }
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
    if (const auto* provider_scope = domain.provider()) {
      const auto* current = adopted == runtime_route_configuration_readbacks_.end()
                                ? nullptr : adopted->second.provider();
      if (current == nullptr || current->domain != domain ||
          current->provider_occurrence != provider ||
          current->component_lifecycle_generation != predecessor.component_lifecycle_generation ||
          current->retained_bundle_id != admission.active_projection_revision.retained_bundle_id ||
          current->graph_revision != admission.active_projection_revision.graph_revision ||
          provider_scope->provider.controllable_component_id != component ||
          provider.provider_runtime_unit_id != admission.execution_host_epoch.runtime_unit_id ||
          provider.provider_runtime_epoch != admission.execution_host_epoch.runtime_epoch) {
        return reject<void>("selected bring-up provider predecessor is not adopted",
                            "selected_bring_up_configuration_not_current");
      }
      current_resources.insert(current_resources.end(), provider_scope->output_resources.begin(),
                               provider_scope->output_resources.end());
      continue;
    }
    if (!domain.controller().active_control_configuration ||
        adopted == runtime_route_configuration_readbacks_.end() || adopted->second.provider() != nullptr ||
        adopted->second.controller().active_configuration != *domain.controller().active_control_configuration) {
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
          std::find_if(domain.control_bindings().begin(),
                       domain.control_bindings().end(), [&](const auto& row) {
                         return row.control_binding_id == path.control_binding_id &&
                                row.control_binding_revision == path.control_binding_revision &&
                                row.control_route_id == path.control_route_id;
                       });
      if (binding.retained_bundle_id !=
              admission.active_projection_revision.retained_bundle_id ||
          binding.graph_revision !=
              admission.active_projection_revision.graph_revision ||
          path.control_domain_revision != domain.control_domain_revision ||
          path.controller_topology_id != domain.controller().controller_topology_id ||
          path.topology_revision != domain.controller().topology_revision ||
          control_binding == domain.control_bindings().end() ||
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
    for (const auto& binding : domain.control_bindings()) {
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
    expected_resources.insert(expected_resources.end(), domain.resource_ids().begin(),
                              domain.resource_ids().end());
  std::sort(expected_resources.begin(), expected_resources.end());
  expected_resources.erase(std::unique(expected_resources.begin(), expected_resources.end()),
                           expected_resources.end());
  if (current_resources != expected_resources) {
    return reject<void>("selected bring-up predecessor physical closure changed",
                        "selected_bring_up_resources_not_current");
  }
  return validate_bring_up_candidate_peer_closure_locked(component, candidates,
                                                         candidate_provider_domains, admission);
}

Result<void> ResourceAddressedExecutionKernel::validate_bring_up_candidate_peer_closure_locked(
    const ControllableComponentId& component,
    const std::vector<RuntimeParticipantBindingDTO>& candidates,
    const std::vector<ResolvedLifecycleControlDomainScopeDTO>& candidate_provider_domains,
    const ConfigurationFenceAdmissionSnapshotDTO& admission) {
  std::optional<ControlDomainId> previous;
  for (const auto& domain : candidate_provider_domains) {
    const auto* provider = domain.provider();
    if (!domain.locally_valid() || !provider ||
        provider->provider.controllable_component_id != component ||
        provider->provider.provider_runtime_unit_id !=
            admission.execution_host_epoch.runtime_unit_id ||
        provider->provider_runtime_epoch != admission.execution_host_epoch.runtime_epoch ||
        (previous && !(*previous < domain.control_domain_id))) {
      return reject<void>("selected bring-up provider candidate is not exact",
                          "selected_bring_up_candidate_provider_invalid",
                          ErrorCode::kInvalidRequest);
    }
    previous = domain.control_domain_id;
    for (const auto& [id, readback] : runtime_route_configuration_readbacks_) {
      const auto* other = readback.provider();
      if (!other || other->domain.provider()->provider.controllable_component_id == component)
        continue;
      if (id == domain.control_domain_id ||
          std::any_of(provider->output_resources.begin(), provider->output_resources.end(),
                      [&](const auto& resource) {
                        return std::binary_search(other->domain.resource_ids().begin(),
                                                  other->domain.resource_ids().end(), resource);
                      })) {
        return reject<void>("selected bring-up crosses a healthy provider peer",
                            "selected_bring_up_nonselected_scope_changed");
      }
    }
    for (const auto& registration : registrations_) {
      const auto& other = registration.binding.value();
      if (other.controllable_component_id == component) continue;
      if ((other.control_path &&
           other.control_path->control_domain_id == domain.control_domain_id) ||
          std::any_of(other.resources.begin(), other.resources.end(), [&](const auto& resource) {
            return std::binary_search(provider->output_resources.begin(),
                                      provider->output_resources.end(), resource.resource_id);
          })) {
        return reject<void>("selected bring-up crosses a healthy peer domain or resource",
                            "selected_bring_up_nonselected_scope_changed");
      }
    }
  }
  for (const auto& candidate : candidates) {
    if (candidate.io_output_binding) {
      const auto& output = *candidate.io_output_binding;
      const auto found =
          std::find_if(candidate_provider_domains.begin(), candidate_provider_domains.end(),
                       [&](const auto& domain) {
                         return domain.control_domain_id == output.provider.control_domain_id;
                       });
      if (found == candidate_provider_domains.end() ||
          candidate.controllable_component_id != component ||
          !ProviderDomainMatchesOccurrence(
              found->control_domain_id, found->control_domain_revision, found->provider()->provider,
              found->provider()->provider_runtime_epoch, output.provider) ||
          output.signal_configuration_digest !=
              found->provider()->provider.signal_configuration_digest ||
          !output.signal.output_resource ||
          !std::binary_search(found->resource_ids().begin(), found->resource_ids().end(),
                              *output.signal.output_resource)) {
        return reject<void>("selected bring-up output candidate has no exact provider domain",
                            "selected_bring_up_candidate_provider_invalid",
                            ErrorCode::kInvalidRequest);
      }
      continue;
    }
    if (!candidate.control_path)
      return reject<void>("selected bring-up candidate has no actual domain authority",
                          "selected_bring_up_candidate_invalid", ErrorCode::kInvalidRequest);
    const auto& path = *candidate.control_path;
    if (path.provider_runtime_unit_id !=
            admission.execution_host_epoch.runtime_unit_id ||
        path.provider_runtime_epoch != admission.execution_host_epoch.runtime_epoch) {
      return reject<void>("selected bring-up cannot change the shared host epoch",
                          "selected_bring_up_host_epoch_changed");
    }
    for (const auto& [id, readback] : runtime_route_configuration_readbacks_) {
      const auto* other = readback.provider();
      if (!other || other->domain.provider()->provider.controllable_component_id == component)
        continue;
      if (id == path.control_domain_id ||
          std::any_of(
              candidate.resources.begin(), candidate.resources.end(), [&](const auto& resource) {
                return std::binary_search(other->domain.resource_ids().begin(),
                                          other->domain.resource_ids().end(), resource.resource_id);
              }))
        return reject<void>("selected bring-up crosses a healthy provider peer",
                            "selected_bring_up_nonselected_scope_changed");
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
  const auto attach_provider = [](ConfigurationFenceDomainScopeDTO& target,
                                  const ResolvedLifecycleControlDomainScopeDTO& scope,
                                  bool candidate) {
    const auto* provider = scope.provider();
    if (!provider) return true;
    if (!std::holds_alternative<ProviderConfigurationFenceDomainDTO>(target.payload))
      target.payload = ProviderConfigurationFenceDomainDTO{};
    auto& typed = std::get<ProviderConfigurationFenceDomainDTO>(target.payload);
    auto& side = candidate ? typed.candidate_provider : typed.predecessor_provider;
    const ProviderConfigurationFenceOccurrenceDTO proof{
        provider->provider.controllable_component_id, provider->provider.provider_instance_id,
        provider->provider.signal_configuration_digest, provider->output_resources};
    if (side && *side != proof) return false;
    side = proof;
    return true;
  };
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
    if ((row.candidate_participants.empty() && row.candidate_provider_domains.empty()) ||
        row.candidate_participants.size() > kMaximumCellAdmissionScopeEntries ||
        row.candidate_provider_domains.size() > kMaxLifecycleResolvedDomains) {
      return reject<void>("Apply bring-up candidate closure is absent or exceeds its bound",
                          "apply_bring_up_candidate_invalid", ErrorCode::kInvalidRequest);
    }
    for (const auto& candidate : row.candidate_participants) {
      const auto verified = verify_runtime_participant_binding(candidate);
      if (!verified.ok() || candidate.controllable_component_id != component ||
          (!candidate.control_path && !candidate.io_output_binding)) {
        return reject<void>("Apply bring-up candidate binding is not qualified",
                            "apply_bring_up_candidate_invalid", ErrorCode::kInvalidRequest);
      }
      // Preserve derive_stop_static_facts' existing physical attribution:
      // same-component aliases are valid; another component cannot own it.
      for (const auto& resource : candidate.resources) {
        if (candidate.io_output_binding) continue;
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
          std::any_of(registrations_.begin(), registrations_.end(),
                      [&](const auto& current) {
                        return current.binding.value().controllable_component_id == component;
                      }) ||
          std::any_of(runtime_route_configuration_readbacks_.begin(),
                      runtime_route_configuration_readbacks_.end(),
                      [&](const auto& current) {
                        const auto* provider = current.second.provider();
                        return provider &&
                               provider->domain.provider()->provider.controllable_component_id ==
                                   component;
                      }) ||
          std::any_of(incident_containments_.begin(), incident_containments_.end(),
                      [&](const auto& current) {
                        return current.controllable_component_id == component;
                      })) {
        return reject<void>("initial bring-up has an installed or faulted predecessor",
                            "apply_bring_up_initial_not_current");
      }
      if (auto current = validate_bring_up_candidate_peer_closure_locked(
              component, row.candidate_participants, row.candidate_provider_domains,
              offer.admission);
          !current.ok())
        return current;
    } else {
      if (row.kind != ApplyBringUpAdmissionComponentKindDTO::kContainedRecovery ||
          !row.predecessor || !row.recovery ||
          (row.candidate_participants.empty() && row.candidate_provider_domains.empty()) ||
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
              predecessor, row.candidate_participants, row.candidate_provider_domains,
              offer.admission);
          !current.ok())
        return current;
      for (const auto& domain : predecessor.domains) {
        auto& entry = domains.try_emplace(
            domain.control_domain_id,
            ConfigurationFenceDomainScopeDTO{domain.control_domain_id, {}, {}}).first->second;
        entry.control_domain_id = domain.control_domain_id;
        entry.predecessor_control_domain_revision = domain.control_domain_revision;
        if (!attach_provider(entry, domain, false))
          return reject<void>("predecessor provider domain closure conflicts",
                              "apply_bring_up_candidate_invalid");
        for (const auto& binding : domain.control_bindings()) {
          auto& current = bindings.try_emplace(
              binding.control_binding_id,
              ConfigurationFenceBindingScopeDTO{binding.control_binding_id, {}, {}}).first->second;
          current.control_binding_id = binding.control_binding_id;
          current.predecessor_control_binding_revision = binding.control_binding_revision;
        }
        resources.insert(domain.resource_ids().begin(), domain.resource_ids().end());
      }
    }
    for (const auto& candidate : row.candidate_provider_domains) {
      auto& domain =
          domains
              .try_emplace(candidate.control_domain_id,
                           ConfigurationFenceDomainScopeDTO{candidate.control_domain_id, {}, {}})
              .first->second;
      if (domain.candidate_control_domain_revision &&
          domain.candidate_control_domain_revision != candidate.control_domain_revision)
        return reject<void>("candidate provider domain closure conflicts",
                            "apply_bring_up_candidate_invalid");
      domain.candidate_control_domain_revision = candidate.control_domain_revision;
      if (!attach_provider(domain, candidate, true))
        return reject<void>("candidate provider domain closure conflicts",
                            "apply_bring_up_candidate_invalid");
      resources.insert(candidate.resource_ids().begin(), candidate.resource_ids().end());
    }
    for (const auto& candidate : row.candidate_participants) {
      if (candidate.io_output_binding) continue;
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
  // D6: a latch carried past Close constrains only motion, not the next
  // Scenario's bring-up.
  const auto fault_scope = stop_fault_admission_scope_locked();
  if ((fault_scope != StopFaultAdmissionScope::kNone &&
       fault_scope != StopFaultAdmissionScope::kCarried && !exact_latch) ||
      closure != offer.admission.echoed_query.affected_scope) {
    return reject<void>("Apply bring-up crosses its exact contained selected scope",
                        "apply_bring_up_scope_not_exact");
  }
  return Result<void>::Ok();
}

Result<CellAdmissionClaimDTO>
ResourceAddressedExecutionKernel::AcquireConfigurationApplyCellAdmissionClaim(
    const CellAdmissionClaimIntentDTO& intent,
    const CellAdmissionConfigurationApplyValidator& validate, const bool canonical_empty_candidate) {
  std::lock_guard submit_lock(submission_mutex_);
  std::lock_guard admission_lock(*admission_mutex_);
  return cell_admission_owner_->AcquireConfigurationApplyClaim(intent, validate, canonical_empty_candidate);
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
  detail::AdmissionTiming timing(true, release.request_id.value());
  const auto wait_start = detail::AdmissionTiming::Clock::now();
  std::unique_lock admission_lock(*admission_mutex_);
  timing.record->admission_wait += detail::AdmissionTiming::Clock::now() - wait_start;
  auto released = cell_admission_owner_->ReleaseCellAdmissionClaim(release);
  if (released.ok()) {
    const auto refresh_start = detail::AdmissionTiming::Clock::now();
    refresh_execution_claims();
    timing.record->refresh += detail::AdmissionTiming::Clock::now() - refresh_start;
  }
  admission_lock.unlock();
  return released;
}

Result<AuthoringAdmissionDTO> ResourceAddressedExecutionKernel::ReconcileCellAdmissionClaims(
    const CellAdmissionReconcileDTO& reconcile) {
  std::unique_lock admission_lock(*admission_mutex_);
  auto reconciled = cell_admission_owner_->ReconcileCellAdmissionClaims(reconcile);
  if (reconciled.ok()) {
    refresh_execution_claims();
  }
  admission_lock.unlock();
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
    if (program_cycle_claim_run_) mark_motion_retry_run_end_locked(program_cycle_claim_run_->program_run_id);
    retired = std::move(program_cycle_claim_run_);
    program_cycle_claim_run_.reset();
  } else if (!program_cycle_claim_run_.has_value() ||
             program_cycle_claim_run_->claim_generation != claim->claim_generation) {
    // A new held Run changes dispatch authority immediately, but the previous
    // factual value store stays installed until its qualified baseline commits.
    // Exact Run-bound consumers cannot borrow that predecessor occurrence.
    if (program_cycle_claim_run_) mark_motion_retry_run_end_locked(program_cycle_claim_run_->program_run_id);
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
          (reason->second == "operation_not_resident" ||
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
  auto proof = cell_admission_owner_->PrepareProgramResetSceneAuthorization(authorization);
  if (!proof.ok()) return Result<void>::Err(proof.error());
  std::lock_guard admission_lock(*admission_mutex_);
  return cell_admission_owner_->ValidatePreparedProgramResetSceneAuthorization(authorization,
                                                                            proof.value());
}

Result<void> ResourceAddressedExecutionKernel::ValidateProgramResetPositionAuthorization(
    const ProgramResetPositionAuthorizationDTO& authorization) const {
  auto proof = cell_admission_owner_->PrepareProgramResetPositionAuthorization(authorization);
  if (!proof.ok()) return Result<void>::Err(proof.error());
  std::lock_guard submission_lock(submission_mutex_);
  std::lock_guard admission_lock(*admission_mutex_);
  {
    std::lock_guard state_lock(state_mutex_);
    if (pending_program_run_admission_ || !active_operations_.empty() ||
        operation_control_reconfiguration_busy_locked() || motion_admission_unreconciled_ ||
        manager_liveness_motion_inhibited_)
      return reject<void>("Program Reset position effect requires settled work",
                          "program_reset_effects_not_settled");
    for (const auto& slot : resource_slots_->snapshot()) {
      if (slot.state != ResourceSlotState::kFree || slot.owner)
        return reject<void>("Program Reset position effect requires free resources",
                            "program_reset_resources_not_settled");
    }
  }
  return cell_admission_owner_->ValidatePreparedProgramResetPositionAuthorization(authorization,
                                                                                proof.value());
}

Result<void> ResourceAddressedExecutionKernel::notify_io_rule_evidence_processing(
    const std::shared_ptr<IIoSignalService>& affected_service) const {
  if (!affected_service || !config_.io_rule_evidence_processing_notification)
    return Result<void>::Ok();
  try {
    config_.io_rule_evidence_processing_notification(affected_service);
    return Result<void>::Ok();
  } catch (...) {
    return Result<void>::Err({"internal_error", "Unable to queue affected I/O rule processing",
        {{"reason", "io_rule_evidence_processing_notification_failed"}}});
  }
}

Result<void> ResourceAddressedExecutionKernel::Publish(
    const ManagerLivenessObservationDTO& observation) {
  // Deliberately bypasses operation admission. Notify only after the owner's
  // health/transition mutexes and the invalidation's state/signal locks unwind.
  std::shared_ptr<IIoSignalService> affected_service;
  auto notification = Result<void>::Ok();
  auto result = [&] {
    const ScopeExit notify([&] {
      notification = notify_io_rule_evidence_processing(affected_service);
    });
    return cell_admission_owner_->Publish(observation, &affected_service);
  }();
  if (!notification.ok()) {
    if (result.ok()) return notification;
    auto error = result.error();
    error.details.emplace("io_rule_notification_error", notification.error().code);
    return Result<void>::Err(std::move(error));
  }
  return result;
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

void ResourceAddressedExecutionKernel::retire_program_io_watches_locked(
    const ProgramRunId& run_id) {
  if (!program_cycle_claim_run_ || program_cycle_claim_run_->program_run_id != run_id ||
      program_cycle_claim_run_->io_watches_retired)
    return;
  auto& run = *program_cycle_claim_run_;
  // Close registration first, under the same Kernel -> signals lock order.
  run.io_watches_retired = true;
  run.io_watch_scopes.clear();
  if (io_signal_service_) {
    // The installed service must accept an exact owner/run previously accepted
    // by RegisterLevelWatch. An over-bound identity cannot own any such watch.
    (void)io_signal_service_->ReleaseRunWatches(run.program_runner_owner_epoch, run.program_run_id);
  }
}

Result<void> ResourceAddressedExecutionKernel::InstallProgramIoSignalService(
    std::shared_ptr<IIoSignalService> service, const RetainedBundleId& expected_retained_bundle_id,
    const GraphRevision& expected_graph_revision) {
  std::lock_guard state_lock(state_mutex_);
  if (!stop_retained_bundle_id_ || !stop_graph_revision_ ||
      *stop_retained_bundle_id_ != expected_retained_bundle_id ||
      *stop_graph_revision_ != expected_graph_revision) {
    return reject<void>("I/O service does not name the current retained activation",
                        "io_signal_service_context_stale", ErrorCode::kStaleGeneration);
  }
  if (service == io_signal_service_) return Result<void>::Ok();
  if (global_io_rule_applied_context_) {
    return reject<void>("Current global catalog requires its atomic Applied service transition",
                        "io_signal_service_global_catalog_active",
                        ErrorCode::kInvalidStateTransition);
  }
  if (has_active_io_rules_locked()) {
    return reject<void>("Current interrupt rules must retire before I/O service replacement",
                        "io_signal_service_rules_active", ErrorCode::kInvalidStateTransition);
  }
  if (program_cycle_claim_run_ && program_cycle_claim_run_->capture) {
    return reject<void>("Current Program capture must retire before I/O service replacement",
                        "io_signal_service_program_active", ErrorCode::kInvalidStateTransition);
  }
  const auto retired = retire_program_blackboard_source_locked();
  if (!retired.ok()) return retired;
  applied_program_blackboard_context_.reset();
  program_blackboard_source_.reset();
  program_blackboard_source_retired_ = false;
  io_signal_service_ = std::move(service);
  return Result<void>::Ok();
}

Result<void> ResourceAddressedExecutionKernel::validate_current_program_io_run_scope_locked(
    const ProgramRunMockSimulationScopeDTO& request) const {
  if (request.request_owner_epoch != config_.request_owner_epoch || !program_cycle_claim_run_ ||
      !program_cycle_claim_run_->capture || !program_cycle_claim_run_->capture->binding ||
      !program_cycle_claim_run_->capture->table ||
      request.program_runner_owner_epoch != program_cycle_claim_run_->program_runner_owner_epoch ||
      request.program_run_id != program_cycle_claim_run_->program_run_id ||
      request.claim_generation != program_cycle_claim_run_->claim_generation ||
      request.program_binding_id != program_cycle_claim_run_->capture->program_binding_id) {
    return reject<void>("I/O watch does not name the current captured Run/claim",
                        "io_watch_scope_stale", ErrorCode::kStaleGeneration);
  }
  return Result<void>::Ok();
}

Result<void> ResourceAddressedExecutionKernel::validate_current_program_io_node_scope_locked(
    const ProgramIoWatchScopeReleaseRequestDTO& request) const {
  const auto current = validate_current_program_io_run_scope_locked(
      {request.request_owner_epoch, request.program_binding_id, request.program_runner_owner_epoch,
       request.claim_generation, request.program_run_id});
  if (!current.ok()) return current;
  if (!valid_program_node_execution_scope(request.node)) {
    return reject<void>("I/O watch node scope is malformed", "io_watch_scope_invalid",
                        ErrorCode::kInvalidRequest);
  }
  const auto& nodes = program_cycle_claim_run_->capture->table->nodes;
  const auto node =
      std::lower_bound(nodes.begin(), nodes.end(), request.node.node_instance_id,
                       [](const auto& row, const auto& id) { return row.instance_id < id; });
  if (node == nodes.end() || node->instance_id != request.node.node_instance_id)
    return reject<void>("Captured Program does not contain this node", "io_watch_node_unsupported",
                        ErrorCode::kInvalidRequest);
  return Result<void>::Ok();
}

Result<void> ResourceAddressedExecutionKernel::validate_program_io_watch_scope_locked(
    const ProgramIoWatchScopeReleaseRequestDTO& request) const {
  const auto current = validate_current_program_io_node_scope_locked(request);
  if (!current.ok()) return current;
  const auto& nodes = program_cycle_claim_run_->capture->table->nodes;
  const auto node = std::lower_bound(nodes.begin(), nodes.end(), request.node.node_instance_id,
      [](const auto& row, const auto& id) { return row.instance_id < id; });
  if (!std::any_of(
          node->target_effects.begin(), node->target_effects.end(), [](const auto& effect) {
            return effect.capability_id.value() == "io.gpio.wait_for_input" && !effect.effectful;
          })) {
    return reject<void>("Captured node does not declare an I/O watch", "io_watch_node_unsupported",
                        ErrorCode::kInvalidRequest);
  }
  return Result<void>::Ok();
}

Result<void> ResourceAddressedExecutionKernel::SetProgramRunMockSimulationPaused(
    const ProgramRunMockSimulationPauseRequestDTO& request) {
  // SAME current Run lock as START/END and actual Run retirement. The service
  // alone retains revision ordering and the host-monotonic remaining delays.
  std::lock_guard state_lock(state_mutex_);
  const auto current = validate_current_program_io_run_scope_locked(request.scope);
  if (!current.ok()) return current;
  if (request.run_revision == 0U)
    return reject<void>("Mock simulation Run revision is unnumbered", "mock_simulation_invalid", ErrorCode::kInvalidRequest);
  if (program_cycle_claim_run_->io_watches_retired)
    return reject<void>("Mock simulation Run has retired", "mock_simulation_scope_stale", ErrorCode::kStaleGeneration);
  const auto& nodes = program_cycle_claim_run_->capture->table->nodes;
  if (request.held_node_instance_ids.size() > nodes.size())
    return reject<void>("Mock simulation held mask exceeds its actual capture", "mock_simulation_invalid", ErrorCode::kInvalidRequest);
  for (std::size_t index = 0U; index < request.held_node_instance_ids.size(); ++index) {
    const auto& id = request.held_node_instance_ids[index];
    if (id.empty() || id.value().size() > 128U || id.value().find('\0') != std::string::npos ||
        (index != 0U && !(request.held_node_instance_ids[index - 1U] < id)))
      return reject<void>("Mock simulation held mask is not canonical", "mock_simulation_invalid", ErrorCode::kInvalidRequest);
    const auto found = std::lower_bound(nodes.begin(), nodes.end(), id,
        [](const auto& node, const auto& identity) { return node.instance_id < identity; });
    if (found == nodes.end() || found->instance_id != id)
      return reject<void>("Mock simulation held node is absent from its actual capture", "mock_simulation_node_unknown", ErrorCode::kInvalidRequest);
  }
  if (!io_signal_service_)
    return reject<void>("I/O signal service is not installed", "io_signal_service_unavailable", ErrorCode::kUnsupportedCapability);
  return io_signal_service_->SetProgramRunMockSimulationPaused(request);
}

Result<void> ResourceAddressedExecutionKernel::EndProgramNodeMockSimulation(
    const ProgramNodeMockSimulationScopeDTO& scope) {
  std::lock_guard state_lock(state_mutex_);
  const auto current = validate_current_program_io_node_scope_locked(
      {scope.request_owner_epoch, scope.program_binding_id, scope.program_runner_owner_epoch,
       scope.claim_generation, scope.program_run_id, scope.node});
  if (!current.ok()) return current;
  if (scope.node_start_index == 0U)
    return reject<void>("Mock simulation end is unnumbered", "mock_simulation_invalid", ErrorCode::kInvalidRequest);
  const auto& run = *program_cycle_claim_run_;
  if (!io_signal_service_)
    return reject<void>("I/O signal service is not installed", "io_signal_service_unavailable", ErrorCode::kUnsupportedCapability);
  if (run.io_watches_retired) {
    // Only this already owner-retired, still-exact captured Run can obtain
    // its cleanup receipt here. Retry the SAME service with retained IDs:
    // the retired flag alone is not proof of successful cancellation.
    return io_signal_service_->ReleaseRunWatches(
        run.program_runner_owner_epoch, run.program_run_id);
  }
  return io_signal_service_->EndProgramNodeMockSimulation(scope, run.capture->table->nodes.size());
}

Result<ProgramNodeMockSimulationAcknowledgementDTO>
ResourceAddressedExecutionKernel::StartProgramNodeMockSimulation(
    const ProgramNodeMockSimulationRequestDTO& request) {
  using Reply = Result<ProgramNodeMockSimulationAcknowledgementDTO>;
  // SAME current Run owner serializes source mutation against authoritative
  // ReleaseRunWatches retirement. No duplicated index/replay state lives here.
  std::lock_guard state_lock(state_mutex_);
  const auto& scope = request.scope;
  const auto current = validate_current_program_io_node_scope_locked(
      {scope.request_owner_epoch, scope.program_binding_id, scope.program_runner_owner_epoch,
       scope.claim_generation, scope.program_run_id, scope.node});
  if (!current.ok()) return Reply::Err(current.error());
  if (scope.node_start_index == 0U || request.signals.empty())
    return reject<ProgramNodeMockSimulationAcknowledgementDTO>(
        "Mock simulation start is empty or unnumbered", "mock_simulation_invalid", ErrorCode::kInvalidRequest);
  const auto& run = *program_cycle_claim_run_;
  if (run.io_watches_retired)
    return reject<ProgramNodeMockSimulationAcknowledgementDTO>(
        "Mock simulation Run has retired", "mock_simulation_scope_stale", ErrorCode::kStaleGeneration);
  if (!io_signal_service_)
    return reject<ProgramNodeMockSimulationAcknowledgementDTO>(
        "I/O signal service is not installed", "io_signal_service_unavailable", ErrorCode::kUnsupportedCapability);
  const auto& capture = *run.capture;
  const auto node = std::lower_bound(capture.table->nodes.begin(), capture.table->nodes.end(),
      scope.node.node_instance_id, [](const auto& row, const auto& id) { return row.instance_id < id; });
  const ProgramNodeIoConditionDTO* node_condition = nullptr;
  if (capture.io_conditions) {
    const auto found = std::find_if(capture.io_conditions->program_node_conditions.begin(),
        capture.io_conditions->program_node_conditions.end(), [&](const auto& row) {
          return row.node_instance_id == scope.node.node_instance_id;
        });
    if (found != capture.io_conditions->program_node_conditions.end()) node_condition = &*found;
  }
  std::vector<ResolvedProgramMockSimulationSignalDTO> resolved;
  resolved.reserve(request.signals.size());
  for (std::size_t index = 0; index < request.signals.size(); ++index) {
    const auto& row = request.signals[index];
    if (index != 0U) {
      const auto& previous = request.signals[index - 1U];
      if (!(previous.target_role_id < row.target_role_id ||
            (previous.target_role_id == row.target_role_id && previous.resource_role_id < row.resource_role_id)))
        return reject<ProgramNodeMockSimulationAcknowledgementDTO>(
            "Mock simulation selectors must be sorted and unique", "mock_simulation_invalid", ErrorCode::kInvalidRequest);
    }
    const auto& targets = capture.binding->target_role_bindings;
    const auto target = std::lower_bound(targets.begin(), targets.end(), row.target_role_id,
        [](const auto& value, const auto& id) { return value.target_role_id < id; });
    if (target == targets.end() || target->target_role_id != row.target_role_id)
      return reject<ProgramNodeMockSimulationAcknowledgementDTO>(
          "Mock simulation target is unbound", "io_signal_unbound", ErrorCode::kInvalidRequest);
    const auto& signals = target->signal_role_bindings;
    const auto signal = std::lower_bound(signals.begin(), signals.end(), row.resource_role_id,
        [](const auto& value, const auto& id) { return value.resource_role_id < id; });
    if (signal == signals.end() || signal->resource_role_id != row.resource_role_id ||
        signal->direction != IoSignalDirection::kInput || signal->value_type != IoSignalValueType::kBoolean)
      return reject<ProgramNodeMockSimulationAcknowledgementDTO>(
          "Mock simulation requires a bound boolean input", "io_signal_unbound", ErrorCode::kInvalidRequest);
    const IoSignalId id{target->controllable_component_id, signal->signal_name};
    const bool checked_condition_input = node_condition && node_condition->condition &&
        std::any_of(node_condition->condition->inputs().begin(), node_condition->condition->inputs().end(),
                    [&](const auto& input) {
                      const auto* actual = std::get_if<IoSignalId>(&input.source);
                      return actual && *actual == id;
                    });
    const bool direct_read_input = std::any_of(node->target_effects.begin(), node->target_effects.end(),
        [&](const auto& effect) {
          return !effect.effectful && effect.target_role_id == row.target_role_id &&
              (effect.capability_id.value() == "io.gpio.get_input" ||
               effect.capability_id.value() == "io.gpio.wait_for_input");
        });
    if (!checked_condition_input && !direct_read_input)
      return reject<ProgramNodeMockSimulationAcknowledgementDTO>(
          "Mock simulation input is not referenced by the captured node", "mock_simulation_node_mismatch", ErrorCode::kInvalidRequest);
    const auto observation = io_signal_service_->ReadSignal(id);
    if (!observation.ok()) return Reply::Err(observation.error());
    const auto digest = ComputeIoSignalDescriptorDigest(observation.value().descriptor);
    if (!digest.ok()) return Reply::Err(digest.error());
    if (observation.value().cursor.owner_epoch != config_.request_owner_epoch ||
        observation.value().cursor.catalog_generation == 0U || digest.value() != signal->signal_descriptor_digest)
      return reject<ProgramNodeMockSimulationAcknowledgementDTO>(
          "Mock simulation source differs from the sealed binding", "io_signal_descriptor_stale", ErrorCode::kStaleGeneration);
    resolved.push_back({id, digest.value(), observation.value().cursor.catalog_generation,
                        observation.value().installed_incarnation});
  }
  return io_signal_service_->StartProgramNodeMockSimulation(request, resolved, capture.table->nodes.size());
}

Result<IoWatchRegistrationDTO> ResourceAddressedExecutionKernel::RegisterProgramIoWatch(
    const ProgramIoWatchRequestDTO& request) {
  std::lock_guard state_lock(state_mutex_);
  const auto scope_valid = validate_program_io_watch_scope_locked(
      {request.request_owner_epoch, request.program_binding_id, request.program_runner_owner_epoch,
       request.claim_generation, request.program_run_id, request.node});
  if (!scope_valid.ok()) return Result<IoWatchRegistrationDTO>::Err(scope_valid.error());
  if (program_cycle_claim_run_->io_watches_retired) {
    return reject<IoWatchRegistrationDTO>("Program watch registration has retired",
                                          "io_watch_run_retired",
                                          ErrorCode::kInvalidStateTransition);
  }
  const auto& capture = *program_cycle_claim_run_->capture;
  const auto node = std::lower_bound(
      capture.table->nodes.begin(), capture.table->nodes.end(), request.node.node_instance_id,
      [](const auto& row, const auto& id) { return row.instance_id < id; });
  if (node == capture.table->nodes.end() || node->instance_id != request.node.node_instance_id ||
      !std::any_of(
          node->target_effects.begin(), node->target_effects.end(), [&](const auto& effect) {
            return effect.target_role_id == request.target_role_id &&
                   effect.capability_id.value() == "io.gpio.wait_for_input" && !effect.effectful;
          })) {
    return reject<IoWatchRegistrationDTO>("Captured node does not declare this I/O watch target",
                                          "io_watch_node_unsupported", ErrorCode::kInvalidRequest);
  }
  const auto target =
      std::lower_bound(capture.binding->target_role_bindings.begin(),
                       capture.binding->target_role_bindings.end(), request.target_role_id,
                       [](const auto& row, const auto& id) { return row.target_role_id < id; });
  if (target == capture.binding->target_role_bindings.end() ||
      target->target_role_id != request.target_role_id) {
    return reject<IoWatchRegistrationDTO>("I/O target is absent from the captured binding",
                                          "io_signal_unbound", ErrorCode::kInvalidRequest);
  }
  const auto signal =
      std::lower_bound(target->signal_role_bindings.begin(), target->signal_role_bindings.end(),
                       request.resource_role_id,
                       [](const auto& row, const auto& id) { return row.resource_role_id < id; });
  if (signal == target->signal_role_bindings.end() ||
      signal->resource_role_id != request.resource_role_id) {
    return reject<IoWatchRegistrationDTO>("I/O resource role is absent from the captured binding",
                                          "io_signal_unbound", ErrorCode::kInvalidRequest);
  }
  if (signal->direction != IoSignalDirection::kInput) {
    return reject<IoWatchRegistrationDTO>("I/O output has no qualified independent input readback",
                                          "io_signal_direction_mismatch",
                                          ErrorCode::kUnsupportedCapability);
  }
  if (signal->value_type != IoSignalValueType::kBoolean) {
    return reject<IoWatchRegistrationDTO>("Only boolean level watches are installed",
                                          "io_signal_type_mismatch",
                                          ErrorCode::kUnsupportedCapability);
  }
  if (!io_signal_service_) {
    return reject<IoWatchRegistrationDTO>("I/O signal service is not installed",
                                          "io_signal_service_unavailable",
                                          ErrorCode::kUnsupportedCapability);
  }
  auto& run = *program_cycle_claim_run_;
  if (run.io_watch_admission_closed) {
    return reject<IoWatchRegistrationDTO>("Uncertain watch cancellation exhausted this Run's bound",
                                          "io_watch_scope_capacity_exhausted",
                                          ErrorCode::kInvalidStateTransition);
  }
  if (std::any_of(run.io_watch_scopes.begin(), run.io_watch_scopes.end(), [&](const auto& scope) {
        return !scope.registration && scope.node == request.node;
      })) {
    return reject<IoWatchRegistrationDTO>("This exact node attempt has ended",
                                          "io_watch_scope_released",
                                          ErrorCode::kInvalidStateTransition);
  }
  if (run.io_watch_scopes.size() >= capture.table->nodes.size()) {
    return reject<IoWatchRegistrationDTO>("I/O watch scope reservation bound reached",
                                          "io_watch_scope_capacity_exhausted",
                                          ErrorCode::kInvalidStateTransition);
  }
  const IoSignalId id{target->controllable_component_id, signal->signal_name};
  const auto observation = io_signal_service_->ReadSignal(id);
  if (!observation.ok()) return Result<IoWatchRegistrationDTO>::Err(observation.error());
  const auto digest = ComputeIoSignalDescriptorDigest(observation.value().descriptor);
  if (!digest.ok()) return Result<IoWatchRegistrationDTO>::Err(digest.error());
  if (observation.value().cursor.owner_epoch != config_.request_owner_epoch ||
      digest.value() != signal->signal_descriptor_digest) {
    return reject<IoWatchRegistrationDTO>(
        "Installed I/O descriptor differs from the sealed binding", "io_signal_descriptor_stale",
        ErrorCode::kStaleGeneration);
  }
  // Reserve cancellation capacity before the service can schedule any Mock
  // answer. A known failed registration has no effect and releases its slot.
  run.io_watch_scopes.push_back({request.node, std::nullopt});
  const auto registered = io_signal_service_->RegisterLevelWatch(
      {{request.program_runner_owner_epoch, request.program_run_id, request.node},
       id,
       request.expected_value});
  if (registered.ok()) {
    run.io_watch_scopes.back().registration = registered.value();
  } else {
    run.io_watch_scopes.pop_back();
  }
  return registered;
}

Result<IoWatchResultDTO> ResourceAddressedExecutionKernel::ReadProgramIoWatch(
    const IoWatchRegistrationDTO& registration) const {
  std::lock_guard state_lock(state_mutex_);
  if (!io_signal_service_) {
    return reject<IoWatchResultDTO>("I/O signal service is not installed",
                                    "io_signal_service_unavailable",
                                    ErrorCode::kUnsupportedCapability);
  }
  return io_signal_service_->ReadWatch(registration);
}

Result<void> ResourceAddressedExecutionKernel::ReleaseProgramIoWatch(
    const IoWatchRegistrationDTO& registration) {
  std::lock_guard state_lock(state_mutex_);
  if (!io_signal_service_) {
    return reject<void>("I/O signal service is not installed", "io_signal_service_unavailable",
                        ErrorCode::kUnsupportedCapability);
  }
  const auto released = io_signal_service_->ReleaseWatch(registration);
  if (released.ok() && program_cycle_claim_run_) {
    // The exact known reply proves that this original register preceded its
    // release. Normal attempts do not accumulate a lifetime history budget.
    std::erase_if(program_cycle_claim_run_->io_watch_scopes, [&](const auto& scope) {
      return scope.registration && *scope.registration == registration;
    });
  }
  return released;
}

Result<void> ResourceAddressedExecutionKernel::ReleaseProgramIoWatchScope(
    const ProgramIoWatchScopeReleaseRequestDTO& request) {
  std::lock_guard state_lock(state_mutex_);
  const auto scope_valid = validate_program_io_watch_scope_locked(request);
  if (!scope_valid.ok()) return scope_valid;
  auto& run = *program_cycle_claim_run_;
  if (run.io_watches_retired) return Result<void>::Ok();
  if (!io_signal_service_) {
    return reject<void>("I/O signal service is not installed", "io_signal_service_unavailable",
                        ErrorCode::kUnsupportedCapability);
  }
  // Install the end fence before service calls or allocations can fail. A
  // failed cancellation is not an ACK, but late registration stays closed.
  const auto existing = std::find_if(run.io_watch_scopes.begin(), run.io_watch_scopes.end(),
                                     [&](const auto& scope) { return scope.node == request.node; });
  bool exhausted = false;
  if (existing != run.io_watch_scopes.end()) {
    existing->registration.reset();
    // Duplicate actual registrations share one retained uncertain-scope fence.
    const auto keep = static_cast<std::size_t>(existing - run.io_watch_scopes.begin());
    for (std::size_t index = run.io_watch_scopes.size(); index-- > keep + 1U;) {
      if (run.io_watch_scopes[index].node == request.node)
        run.io_watch_scopes.erase(run.io_watch_scopes.begin() + static_cast<std::ptrdiff_t>(index));
    }
  } else if (run.io_watch_scopes.size() >= run.capture->table->nodes.size()) {
    // A known release must never reopen admission for this unrecorded scope.
    run.io_watch_admission_closed = true;
    exhausted = true;
  } else {
    try {
      run.io_watch_scopes.push_back({request.node, std::nullopt});
    } catch (...) {
      // In particular, allocation failure cannot leave an ended unknown
      // register admissible. Still cancel any actual matching service watch.
      run.io_watch_admission_closed = true;
      exhausted = true;
    }
  }
  const auto released = io_signal_service_->ReleaseNodeWatches(
      {request.program_runner_owner_epoch, request.program_run_id, request.node});
  if (!released.ok()) return released;
  if (exhausted) {
    // Report the actual fault/fence instead of a false clean-cancel ACK.
    // Other watches are unchanged and no motion Stop is invented here.
    return Result<void>::Err({"execution_rejected",
                              "Uncertain watch scope cancellation exhausted this Run's bound",
                              {{"reason", "io_watch_scope_capacity_exhausted"},
                               {"watch_admission_fenced", "true"},
                               {"matching_watches_released", "true"}}});
  }
  return Result<void>::Ok();
}

void ResourceAddressedExecutionKernel::LatchAndContain(
    const ManagerLivenessLossIntentDTO& intent) noexcept {
  std::shared_ptr<IIoSignalService> affected_service;
  // This guard runs after mandatory containment has been attempted and every
  // owner/state scope has unwound, including an invalidation/storage error.
  const ScopeExit notify([&] {
    try {
      static_cast<void>(notify_io_rule_evidence_processing(affected_service));
    } catch (...) {
      // The void fail-safe boundary cannot report queue allocation failure;
      // actual source unavailability and mandatory containment remain intact.
    }
  });
  try {
    std::optional<ErrorDTO> source_invalidation_error;
    if (!cell_admission_owner_->LatchManagerLivenessLoss(
            intent, &source_invalidation_error, &affected_service)) {
      return;
    }
    StopGeneration expected_stop_generation(0U);
    {
      std::lock_guard state_lock(state_mutex_);
      // This is the execution-side fail-safe latch. It is not a second
      // liveness timer or state owner and is deliberately set before Stop I/O.
      manager_liveness_motion_inhibited_ = true;
      motion_admission_closed_ = true;
      // The owner hook already invalidated the exact source before unlocking.
      // A failed loss append stays visible in Source.error/rule loss evidence;
      // it creates no forensic record or proof and cannot suppress Full Stop.
      if (source_invalidation_error) motion_admission_closed_ = true;
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

void ResourceAddressedExecutionKernel::ContainInternalFault(
    const std::string_view reason_code) noexcept {
  bool claimed = false;
  bool contained = false;
  std::string stop_error;
  try {
    StopGeneration expected_stop_generation(0U);
    {
      std::lock_guard state_lock(state_mutex_);
      motion_admission_closed_ = true;
      // One fault per runtime: a repeating throw only counts, so it requests
      // no further Full Stop (no journal growth per event).
      if (internal_fault_) {
        ++internal_fault_->later_count;
        return;
      }
      internal_fault_ = KernelInternalFaultDTO{config_.request_owner_epoch,
                                               std::string(reason_code), false, std::nullopt, 0U};
      claimed = true;
      expected_stop_generation = cell_stop_generation_;
    }
    const auto material = config_.request_owner_epoch.value() + "\ninternal-fault\n" +
                          std::string(reason_code) + "\n" +
                          std::to_string(expected_stop_generation.value());
    FullStopIntentDTO containment{
        PublicRequestContextDTO{RequestId(manyforge::content_identity::sha256_digest(material)),
                                config_.request_owner_epoch, "kernel.internal_fault",
                                std::string(reason_code)},
        expected_stop_generation,
        RequestAttributionDTO{RequestTransportDTO::kInternal, RequestAssuranceDTO::kUnverified,
                              std::nullopt}};
    // A resident Stop is joined. The Stop fence revokes the lookahead permits
    // of every active motion participant (an idle cell has none).
    const auto accepted = accept_stop(EmergencyStopProjectedIntentDTO{std::move(containment)});
    contained = accepted.ok();
    if (!contained) stop_error = accepted.error().details.contains("reason")
                                     ? accepted.error().details.at("reason")
                                     : accepted.error().code;
  } catch (...) {
    stop_error = "containment_stop_exception";
  }
  // The fault record, its runtime-fault fact and its alarm name the outcome.
  if (claimed) {
    try {
      std::lock_guard state_lock(state_mutex_);
      internal_fault_->stop_accepted = contained;
      if (!contained) internal_fault_->stop_refusal_code = stop_error;
    } catch (...) {
    }
  }
  (void)std::fprintf(stderr, "manyforge_fault code=%.*s state=%s%s%s\n",
                     static_cast<int>(reason_code.size()), reason_code.data(),
                     contained ? "contained" : "containment_failed",
                     contained ? "" : " error=", stop_error.c_str());
}

void ResourceAddressedExecutionKernel::attribute_containment_stop_locked(
    const StopAcceptedDTO& stopped, const StopFaultLatchOriginDTO origin) {
  constexpr std::size_t kMaximumAttributedContainmentStops = 64U;
  if (attributed_containment_stops_.size() >= kMaximumAttributedContainmentStops) {
    attributed_containment_stops_.erase(attributed_containment_stops_.begin());
  }
  attributed_containment_stops_.push_back({stopped.stop_id, stopped.stop_generation, origin});
}

Result<void> ResourceAddressedExecutionKernel::PublishComponentStreamHold(
    const ControllableComponentId& component, const std::optional<ErrorDTO>& hold,
    const bool cell_wide) {
  // Same order as readiness publication: readers hold either mutex.
  std::lock_guard admission_lock(*admission_mutex_);
  std::lock_guard state_lock(state_mutex_);
  component_stream_holds_.erase(component);
  cell_stream_holds_.erase(component);
  if (hold) {
    (cell_wide ? cell_stream_holds_ : component_stream_holds_)
        .insert_or_assign(component, *hold);
  }
  return Result<void>::Ok();
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
    const auto remembered = std::find_if(
        incident_containments_.begin(), incident_containments_.end(),
        [&](const auto& row) { return row.incident_id == intent.incident_id; });
    if (remembered != incident_containments_.end() && remembered->receipt) {
      return Result<ComponentIncidentContainmentReceiptDTO>::Ok(*remembered->receipt);
    }
    if (remembered == incident_containments_.end()) {
      constexpr std::size_t kMaximumRememberedIncidentContainments = 64U;
      if (incident_containments_.size() >= kMaximumRememberedIncidentContainments) {
        const auto accepted_containment = std::find_if(
            incident_containments_.begin(), incident_containments_.end(),
            [](const auto& row) { return row.receipt.has_value(); });
        if (accepted_containment == incident_containments_.end()) {
          return reject<ComponentIncidentContainmentReceiptDTO>(
              "All retained incident containments are pending",
              "incident_containment_capacity_exhausted", ErrorCode::kInvalidStateTransition);
        }
        incident_containments_.erase(accepted_containment);
      }
      // The same bounded owner table fences recovery's final admission
      // reopen. Never create a placeholder Stop identity or drop a pending row.
      incident_containments_.push_back({intent.incident_id, std::nullopt, std::nullopt});
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
    const auto remembered = std::find_if(
        incident_containments_.begin(), incident_containments_.end(),
        [&](const auto& row) { return row.incident_id == intent.incident_id; });
    // Pending rows cannot be evicted. A concurrent replay can fill it first;
    // preserve the original acceptance's component attribution in that case.
    if (remembered != incident_containments_.end()) {
      remembered->receipt = receipt;
      if (stopped.value().replay_status == RequestReplayStatusDTO::kAcceptedNew)
        remembered->controllable_component_id = intent.controllable_component_id;
    }
  }
  return Result<ComponentIncidentContainmentReceiptDTO>::Ok(std::move(receipt));
}

Result<bool> ResourceAddressedExecutionKernel::RecoverComponentIncidentContainment(
    const std::string& incident_id) {
  std::optional<ComponentIncidentContainmentReceiptDTO> receipt;
  {
    std::lock_guard state_lock(state_mutex_);
    const auto remembered = std::find_if(
        incident_containments_.begin(), incident_containments_.end(),
        [&](const auto& row) { return row.incident_id == incident_id; });
    if (remembered != incident_containments_.end()) receipt = remembered->receipt;
  }
  // Unknown here (another owner, or never accepted): the operator's.
  if (!receipt) return Result<bool>::Ok(true);
  const auto offer = get_stop_fault_recovery_offer();
  if (!offer.ok()) return Result<bool>::Err(offer.error());
  if (!offer.value()) return Result<bool>::Ok(true);
  const auto& latch = offer.value()->latch;
  // Only a latch this containment alone caused (no joined operator Stop).
  if (latch.stop_id != receipt->stop_id || latch.stop_generation != receipt->stop_generation ||
      get_stop_fault_latch_origin(latch) != StopFaultLatchOriginDTO::kComponentIncident)
    return Result<bool>::Ok(true);
  if (!latch.stop_certificate) return Result<bool>::Ok(false);
  if (offer.value()->configuration_expectations.empty()) return Result<bool>::Ok(true);
  const auto material = config_.request_owner_epoch.value() + "\ncomponent-incident-recovery\n" +
                        incident_id + "\n" + latch.latch_id.value() + "\n" +
                        std::to_string(latch.latch_generation.value()) + "\n" +
                        std::to_string(latch.recovery_assessment_revision);
  const StopFaultRecoveryIntentDTO intent{
      PublicRequestContextDTO{RequestId(content_identity::sha256_digest(material)),
                              config_.request_owner_epoch, "cclc.component_incident",
                              incident_id},
      latch.latch_id,
      latch.latch_generation,
      latch.recovery_assessment_revision,
      latch.stop_certificate->stop_certificate_id,
      latch.stop_certificate->stop_certificate_revision,
      latch.cell_id,
      latch.retained_bundle_id,
      latch.graph_revision,
      offer.value()->configuration_expectations,
      std::string("component stream live again")};
  const auto recovered = recover_stop_fault(intent);
  if (!recovered.ok()) return Result<bool>::Err(recovered.error());
  return Result<bool>::Ok(recovered.value().accepted);
}

Result<AuthoringAdmissionDTO> ResourceAddressedExecutionKernel::ReplaceManagerLivenessRequiredSet(
    const ManagerLivenessRequiredSetDTO& required_set) {
  std::shared_ptr<IIoSignalService> affected_service;
  auto notification = Result<void>::Ok();
  auto result = [&] {
    const ScopeExit notify([&] {
      notification = notify_io_rule_evidence_processing(affected_service);
    });
    return cell_admission_owner_->ReplaceManagerLivenessRequiredSet(required_set, &affected_service);
  }();
  if (!notification.ok()) {
    if (result.ok()) return Result<AuthoringAdmissionDTO>::Err(notification.error());
    auto error = result.error();
    error.details.emplace("io_rule_notification_error", notification.error().code);
    return Result<AuthoringAdmissionDTO>::Err(std::move(error));
  }
  return result;
}

}  // namespace manyforge::core
