#include <manyforge_core/execution_interrupt_support.hpp>

namespace manyforge::core {

bool validation_cancelled_by_interrupt(const ErrorDTO& error) {
  if (error.code != kErrorValidationCollision) {
    return false;
  }
  const auto it = error.details.find("reason");
  return it != error.details.end() &&
         it->second == "collision_validation_cancelled";
}

bool planning_cancelled_by_interrupt(const ErrorDTO& error) {
  if (error.code != kErrorPlanningFailed) {
    return false;
  }
  const auto it = error.details.find("reason");
  return it != error.details.end() &&
         it->second == "planning_cancelled_by_interrupt";
}

bool authoritative_collision_replan_candidate(const ErrorDTO& error) {
  if (error.code != kErrorValidationCollision ||
      validation_cancelled_by_interrupt(error)) {
    return false;
  }
  const auto decision_it = error.details.find("collision_result_decision");
  if (decision_it == error.details.end() || decision_it->second != "fail") {
    return false;
  }
  const auto status_it = error.details.find("collision_result_engine_status");
  return status_it != error.details.end() && status_it->second == "ok";
}

std::optional<KernelEventType> interrupt_event_type_from_name(
    const std::string& name) {
  static const std::map<std::string, KernelEventType> kMap{
      {"interrupt_signal_ingested", KernelEventType::kInterruptSignalIngested},
      {"interrupt_signal_rejected_stale",
       KernelEventType::kInterruptSignalRejectedStale},
      {"interrupt_signal_rejected_class_violation",
       KernelEventType::kInterruptSignalRejectedClassViolation},
      {"interrupt_signal_rejected", KernelEventType::kInterruptSignalRejected},
      {"interrupt_latched", KernelEventType::kInterruptLatched},
      {"interrupt_suppressed_by_active_mask",
       KernelEventType::kInterruptSuppressedByActiveMask},
      {"interrupt_queued", KernelEventType::kInterruptQueued},
      {"interrupt_dropped_queue_overflow",
       KernelEventType::kInterruptDroppedQueueOverflow},
      {"interrupt_preempt_applied", KernelEventType::kInterruptPreemptApplied},
      {"interrupt_disposition_applied",
       KernelEventType::kInterruptDispositionApplied},
      {"interrupt_cleared", KernelEventType::kInterruptCleared},
      {"interrupt_ingest_overflow", KernelEventType::kInterruptIngestOverflow},
      {"interrupt_source_class_violation",
       KernelEventType::kInterruptSourceClassViolation},
      {"interrupt_recovery_handler_started",
       KernelEventType::kInterruptRecoveryHandlerStarted},
      {"interrupt_recovery_handler_completed",
       KernelEventType::kInterruptRecoveryHandlerCompleted},
      {"motion_interrupt_scope_armed",
       KernelEventType::kMotionInterruptScopeArmed},
      {"motion_interrupt_scope_reaped",
       KernelEventType::kMotionInterruptScopeReaped},
      {"motion_interrupt_queued_update_dropped",
       KernelEventType::kMotionInterruptQueuedUpdateDropped},
      {"motion_trip_secondary_signal",
       KernelEventType::kMotionTripSecondarySignal},
      {"motion_trip_failed", KernelEventType::kMotionTripFailed},
      {"motion_trip_completed", KernelEventType::kMotionTripCompleted},
  };
  const auto it = kMap.find(name);
  if (it == kMap.end()) {
    return std::nullopt;
  }
  return it->second;
}

InterruptIngestResult reject_interrupts_without_active_phase(
    const std::vector<InterruptSignalUpdate>& updates) {
  InterruptIngestResult result;
  result.rejected = true;
  result.reason = "no_active_execution_phase";
  for (const auto& update : updates) {
    result.events.push_back(
        {"interrupt_signal_rejected",
         {{"source_id", update.source_id},
          {"sequence_id", std::to_string(update.sequence_id)},
          {"reason", result.reason}},
         std::nullopt});
  }
  return result;
}

ErrorDTO authoritative_collision_validation_config_error(
    std::string reason,
    std::map<std::string, std::string> details,
    std::string phase) {
  details.emplace("reason", std::move(reason));
  details.emplace("phase", std::move(phase));
  return ErrorDTO{
      kErrorInvariantViolation,
      "Authoritative collision validation pipeline is not configured",
      std::move(details)};
}

InterruptSourceDefinition runtime_collision_fault_source_definition() {
  InterruptSourceDefinition source;
  source.source_id = kRuntimeCollisionFaultSourceId;
  source.adapter = InterruptAdapter::kKernel;
  source.signal = "runtime_collision_fault";
  source.source_class = InterruptSourceClass::kFastExecutionRelevant;
  source.kind = InterruptSourceKind::kEdge;
  source.priority = 1000;
  source.maskable = false;
  source.freshness_budget_ns = 1'000'000;
  source.description =
      "Kernel-owned runtime collision monitor fault source";
  return source;
}

InterruptPolicyDefinition runtime_collision_fault_policy_definition() {
  InterruptPolicyDefinition policy;
  policy.policy_id = kRuntimeCollisionFaultPolicyId;
  policy.source_id = kRuntimeCollisionFaultSourceId;
  policy.priority = 1000;
  policy.maskable = false;
  policy.affected_phases = {InterruptExecutionPhase::kTrajectoryExecution};
  policy.preemption_action = InterruptPreemptionAction::kPreemptTrajectory;
  policy.post_latch_disposition =
      InterruptPostLatchDisposition::kAwaitOperatorAck;
  policy.description =
      "Preempt trajectory execution when runtime collision monitoring detects "
      "a collision";
  return policy;
}

}  // namespace manyforge::core
