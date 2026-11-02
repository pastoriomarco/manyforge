#include <manyforge_core/types/program_binding.hpp>

#include "resource_addressed_execution_kernel_internal.hpp"

namespace manyforge::core {
namespace {

ErrorDTO bb_error(const char* reason, bool malformed = false) {
  return {malformed ? "invalid_request" : "stale_generation",
          "Program blackboard does not name the current installed owner",
          {{"reason", reason}}};
}

// The same Runner names a cycle the Manager liveness heartbeat has not yet
// delivered to this Kernel (e.g. the between-Runs baseline right after the
// Runner journals its new Idle cycle). Not admitted and retryable: the
// Runner's bounded not-ready retry resends it once the cycle is observed.
// An older or equal generation, or another Runner epoch, stays stale.
ErrorDTO bb_cycle_not_observed() {
  return {"bb_source_not_ready",
          "Program blackboard names a Program cycle the Kernel has not observed yet",
          {{"reason", "program_bb_cycle_not_observed"}}};
}

bool cycle_not_yet_observed(const IoProgramBlackboardScopeDTO& scope,
                            const ProgramCycleStatusDTO& cycle) {
  return scope.program_runner_owner_epoch == cycle.program_runner_owner_epoch &&
         scope.cycle_generation > cycle.cycle_generation;
}

bool cycle_matches(const IoProgramBlackboardScopeDTO& scope, const ProgramCycleStatusDTO& cycle) {
  return scope.program_runner_owner_epoch == cycle.program_runner_owner_epoch &&
         scope.cycle_generation == cycle.cycle_generation &&
         scope.program_run_id == cycle.program_run_id &&
         scope.claim_generation == cycle.cycle_claim_generation;
}

std::optional<IoConditionValueKind> declared_kind(const std::string& type) {
  if (type == "bool") return IoConditionValueKind::kBoolean;
  if (type == "int64") return IoConditionValueKind::kInt64;
  if (type == "float64") return IoConditionValueKind::kFloat64;
  if (type == "string") return IoConditionValueKind::kString;
  return std::nullopt;
}

bool context_valid(const AppliedProgramBlackboardContextDTO& context) {
  if (context.request_owner_epoch.empty() || context.program_id.empty() ||
      context.program_revision.empty() || context.program_binding_id.empty() ||
      context.retained_bundle_id.empty() || context.graph_revision.empty() ||
      !context.active_publication_generation.nonzero() ||
      context.active_publication_generation.value() > kMaximumKernelJsonInteger ||
      !context.program_binding || !context.blackboard_key_declarations || !context.io_conditions ||
      context.maximum_value_bytes == 0U)
    return false;
  const auto& binding = *context.program_binding;
  if (binding.program_binding_id != context.program_binding_id ||
      binding.program_id != context.program_id ||
      binding.program_revision != context.program_revision ||
      binding.retained_bundle_id != context.retained_bundle_id ||
      binding.graph_revision != context.graph_revision)
    return false;
  const auto& declarations = *context.blackboard_key_declarations;
  std::string previous;
  std::size_t name_bytes = 0U;
  for (const auto& key : context.required_keys) {
    if (key.name.empty() || key.name.find('\0') != std::string::npos ||
        (!previous.empty() && previous >= key.name) ||
        key.name.size() > context.maximum_value_bytes - name_bytes)
      return false;
    name_bytes += key.name.size();
    previous = key.name;
    const auto declaration = std::find_if(declarations.begin(), declarations.end(),
                                          [&](const auto& row) { return row.name == key.name; });
    if (declaration == declarations.end() ||
        declared_kind(declaration->declared_type) != key.value_kind)
      return false;
  }
  for (const auto& key : context.io_conditions->referenced_program_blackboard_keys) {
    const auto installed =
        std::lower_bound(context.required_keys.begin(), context.required_keys.end(), key.name,
                         [](const auto& row, const auto& name) { return row.name < name; });
    if (installed == context.required_keys.end() || *installed != key) return false;
  }
  return true;
}

bool same_context(const AppliedProgramBlackboardContextDTO& left,
                  const AppliedProgramBlackboardContextDTO& right) {
  return left.request_owner_epoch == right.request_owner_epoch &&
         left.active_publication_generation == right.active_publication_generation &&
         left.program_id == right.program_id && left.program_revision == right.program_revision &&
         left.program_binding_id == right.program_binding_id &&
         left.retained_bundle_id == right.retained_bundle_id &&
         left.graph_revision == right.graph_revision &&
         *left.program_binding == *right.program_binding &&
         *left.blackboard_key_declarations == *right.blackboard_key_declarations &&
         left.required_keys == right.required_keys &&
         left.maximum_value_bytes == right.maximum_value_bytes;
}

IoSourceIncarnation source_incarnation(const IoProgramBlackboardScopeDTO& scope) {
  using content_identity::CanonicalValue;
  const CanonicalValue identity(CanonicalValue::Object{
      {"kernel_owner", CanonicalValue(scope.request_owner_epoch.value())},
      {"runner_owner", CanonicalValue(scope.program_runner_owner_epoch.value())},
      {"publication", CanonicalValue(std::to_string(scope.active_publication_generation.value()))},
      {"binding", CanonicalValue(scope.program_binding_id.value())},
      {"cycle", CanonicalValue(std::to_string(scope.cycle_generation))},
      {"claim", scope.claim_generation
                    ? CanonicalValue(std::to_string(scope.claim_generation->value()))
                    : CanonicalValue(nullptr)},
      {"run", scope.program_run_id ? CanonicalValue(scope.program_run_id->value())
                                   : CanonicalValue(nullptr)}});
  return IoSourceIncarnation(
      content_identity::framed_sha256("program-blackboard-source", "1.0", identity));
}

}  // namespace

std::shared_ptr<const AppliedProgramBlackboardContextDTO>
ResourceAddressedExecutionKernel::applied_program_blackboard_context() const {
  std::lock_guard state_lock(state_mutex_);
  return applied_program_blackboard_context_;
}

Result<void> ResourceAddressedExecutionKernel::retire_program_blackboard_source_locked(
    std::shared_ptr<IIoSignalService>* actual_retired_service) {
  if (!program_blackboard_source_ || program_blackboard_source_retired_) return Result<void>::Ok();
  if (!io_signal_service_) return Result<void>::Err(bb_error("program_bb_service_missing"));
  // Notification only: actual source unavailability is set before the service's
  // potentially failing history append. The caller wakes it after all locks.
  if (actual_retired_service) *actual_retired_service = io_signal_service_;
  const auto retired =
      io_signal_service_->RetireProgramBlackboardSource(*program_blackboard_source_);
  if (!retired.ok()) return retired;
  program_blackboard_source_retired_ = true;
  return Result<void>::Ok();
}

Result<void> ResourceAddressedExecutionKernel::reconcile_program_blackboard_cycle_locked(
    const std::optional<ProgramCycleStatusDTO>& cycle,
    std::shared_ptr<IIoSignalService>* actual_retired_service) {
  if (program_blackboard_source_) {
    const auto& prior = program_blackboard_source_->scope;
    // A healthy same-owner Reset/new Run keeps its accepted value store until
    // the actual replacement baseline commits atomically. Readiness is not a
    // fabricated source frame or a permission for new-Run dispatch to use it.
    // A closed (not current) cycle alone is not a teardown: the Manager closes
    // readiness before every Run end and Reset step. Actual Runner contact
    // loss and Manager liveness loss retire through their own paths.
    if (!cycle || !applied_program_blackboard_context_ ||
        prior.program_runner_owner_epoch != cycle->program_runner_owner_epoch ||
        prior.request_owner_epoch != applied_program_blackboard_context_->request_owner_epoch ||
        prior.program_binding_id != applied_program_blackboard_context_->program_binding_id ||
        prior.active_publication_generation !=
            applied_program_blackboard_context_->active_publication_generation)
      return retire_program_blackboard_source_locked(actual_retired_service);
  }
  return Result<void>::Ok();
}

bool ResourceAddressedExecutionKernel::program_blackboard_source_matches_operation_locked(
    const ActiveOperation& operation) const {
  std::optional<ProgramRunId> expected_run;
  const bool captured_node = std::any_of(operation.intent.participants.begin(),
      operation.intent.participants.end(), [](const auto& participant) {
        return participant.execution_scope.has_value();
      });
  // A public bookkeeping Run ID on an unscoped manual request is not a parent
  // claim. Only admitted captured-node/held-intervention facts establish one.
  if (captured_node) expected_run = operation.state.program_run_id;
  if (operation.manual_handover) expected_run = operation.manual_handover->capture.program_run_id;
  const auto parent_claim = manual_operation_claim_ &&
          manual_operation_claim_->intent.owner_transaction_id == operation.state.operation_id.value()
      ? parent_program_cycle_claim(manual_operation_claim_->intent) : std::nullopt;
  if (!expected_run && parent_claim) {
    if (!program_cycle_claim_run_ || program_cycle_claim_run_->claim_generation != *parent_claim)
      return false;
    expected_run = program_cycle_claim_run_->program_run_id;
  }
  if (!expected_run) return true;  // Truly no Run may observe retained factual values.
  if (!program_cycle_claim_run_ || !program_cycle_claim_run_->capture ||
      program_cycle_claim_run_->program_run_id != expected_run ||
      !program_blackboard_source_ || program_blackboard_source_retired_ ||
      !applied_program_blackboard_context_)
    return false;
  const auto& held = *program_cycle_claim_run_;
  if (operation.manual_handover &&
      (operation.manual_handover->capture.claim_generation != held.claim_generation ||
       operation.manual_handover->capture.program_runner_owner_epoch != held.program_runner_owner_epoch))
    return false;
  const auto& source = program_blackboard_source_->scope;
  return source.request_owner_epoch == config_.request_owner_epoch &&
      source.program_run_id == expected_run && source.claim_generation == held.claim_generation &&
      source.program_runner_owner_epoch == held.program_runner_owner_epoch &&
      source.program_binding_id == held.capture->program_binding_id &&
      source.active_publication_generation ==
          applied_program_blackboard_context_->active_publication_generation;
}

bool ResourceAddressedExecutionKernel::program_blackboard_runner_loss_locked(
    const ProgramCycleStatusDTO& cycle) const {
  if (!program_runner_loss_) return false;
  const auto& lost = program_runner_loss_->established_cycle;
  // A first Starting publication may lag the actual held Run. Loss of that
  // actual Runner/cycle cannot be undone by its still-current Idle predecessor.
  return lost.program_runner_owner_epoch == cycle.program_runner_owner_epoch &&
         lost.cycle_generation == cycle.cycle_generation;
}

bool ResourceAddressedExecutionKernel::program_blackboard_scope_current_locked(
    const IoProgramBlackboardScopeDTO& scope, const ProgramCycleStatusDTO& cycle,
    const AppliedProgramBlackboardContextDTO& context) const {
  return context.program_binding &&
         program_blackboard_scope_current_locked(scope, cycle, context.request_owner_epoch,
                                                 context.active_publication_generation,
                                                 *context.program_binding);
}

bool ResourceAddressedExecutionKernel::program_blackboard_scope_current_locked(
    const IoProgramBlackboardScopeDTO& scope, const ProgramCycleStatusDTO& cycle,
    const RequestOwnerEpoch& expected_owner, ActiveBundlePublicationGeneration expected_publication,
    const ProgramBindingDTO& actual_binding) const {
  if (scope.request_owner_epoch != expected_owner ||
      scope.program_binding_id != actual_binding.program_binding_id ||
      scope.active_publication_generation != expected_publication ||
      scope.program_runner_owner_epoch != cycle.program_runner_owner_epoch ||
      scope.cycle_generation != cycle.cycle_generation)
    return false;
  const bool idle_predecessor = cycle.state == ProgramCycleStateDTO::kIdleAfterReset &&
                                !cycle.program_run_id && !cycle.cycle_claim_generation &&
                                !cycle.terminal_or_settlement_reference;
  if (program_cycle_claim_run_) {
    const auto& held = *program_cycle_claim_run_;
    const auto& capture = held.capture;
    if (!scope.program_run_id || !scope.claim_generation || !capture || !capture->binding ||
        held.program_runner_owner_epoch != scope.program_runner_owner_epoch ||
        held.program_run_id != *scope.program_run_id ||
        held.claim_generation != *scope.claim_generation ||
        capture->request_owner_epoch != scope.request_owner_epoch ||
        capture->program_runner_owner_epoch != scope.program_runner_owner_epoch ||
        capture->program_run_id != *scope.program_run_id ||
        capture->claim_generation != *scope.claim_generation ||
        capture->program_binding_id != actual_binding.program_binding_id ||
        capture->program_id != actual_binding.program_id ||
        capture->program_revision != actual_binding.program_revision ||
        capture->retained_bundle_id != actual_binding.retained_bundle_id ||
        capture->graph_revision != actual_binding.graph_revision)
      return false;
    const bool starting_without_claim = cycle.state == ProgramCycleStateDTO::kStarting &&
                                        cycle.program_run_id == scope.program_run_id &&
                                        !cycle.cycle_claim_generation;
    return cycle_matches(scope, cycle) || idle_predecessor || starting_without_claim;
  }
  if (scope.program_run_id) {
    // Finishing a Run never retires its accepted value-store occurrence. The
    // already installed exact source remains factual while Manager catches up;
    // this receipt cannot authorize a new Run or create a fresh source.
    const bool retained = program_blackboard_source_ && !program_blackboard_source_retired_ &&
                          program_blackboard_source_->scope == scope;
    return (retained && (cycle_matches(scope, cycle) || idle_predecessor)) ||
           (cycle.state == ProgramCycleStateDTO::kTerminalAwaitingReset &&
            cycle_matches(scope, cycle));
  }
  if (program_blackboard_source_ && !program_blackboard_source_retired_ &&
      program_blackboard_source_->scope.program_run_id &&
      program_blackboard_source_->scope.program_runner_owner_epoch ==
          scope.program_runner_owner_epoch &&
      program_blackboard_source_->scope.cycle_generation == scope.cycle_generation)
    return false;  // A lagging Idle projection cannot resurrect the predecessor source.
  return cycle_matches(scope, cycle);
}

Result<void> ResourceAddressedExecutionKernel::ValidatePendingProgramBlackboardScope(
    const IoProgramBlackboardScopeDTO& scope, const ProgramBindingDTO& candidate,
    ActiveBundlePublicationGeneration publication,
    const std::shared_ptr<IIoSignalService>& expected_service) {
  if (!publication.nonzero() || publication.value() > kMaximumKernelJsonInteger ||
      candidate.program_binding_id.empty() || candidate.program_id.empty() ||
      candidate.program_revision.empty() || candidate.retained_bundle_id.empty() ||
      candidate.graph_revision.empty() ||
      scope.program_run_id.has_value() != scope.claim_generation.has_value() ||
      (scope.program_run_id && scope.program_run_id->empty()) ||
      (scope.claim_generation && !scope.claim_generation->nonzero()))
    return Result<void>::Err(bb_error("program_bb_scope_stale"));
  std::lock_guard submission_lock(submission_mutex_);
  std::lock_guard admission_lock(*admission_mutex_);
  // A matching pending Apply fence is an observation, never a source grant.
  // Real installation/publication still require the existing fence to clear.
  auto owner = cell_admission_owner_->AcquireProgramBlackboardAdmission();
  if (!owner.ok()) return Result<void>::Err(owner.error());
  std::lock_guard state_lock(state_mutex_);
  const bool staged_service = staged_global_io_rule_catalog_ &&
      staged_global_io_rule_catalog_->catalog &&
      staged_global_io_rule_catalog_->service == expected_service &&
      staged_global_io_rule_catalog_->context ==
          IoRuleAppliedContextDTO{config_.request_owner_epoch, publication,
                                  candidate.retained_bundle_id, candidate.graph_revision} &&
      staged_global_io_rule_catalog_->catalog->program_binding_id == candidate.program_binding_id;
  if (!expected_service || (io_signal_service_ != expected_service && !staged_service) ||
      owner.value().retained_bundle_id() != candidate.retained_bundle_id ||
      stop_retained_bundle_id_ != candidate.retained_bundle_id ||
      stop_graph_revision_ != candidate.graph_revision ||
      program_blackboard_runner_loss_locked(owner.value().cycle()))
    return Result<void>::Err(bb_error("program_bb_scope_stale"));
  if (!program_blackboard_scope_current_locked(scope, owner.value().cycle(),
                                               config_.request_owner_epoch, publication, candidate))
    return Result<void>::Err(
        scope.request_owner_epoch == config_.request_owner_epoch &&
                scope.program_binding_id == candidate.program_binding_id &&
                scope.active_publication_generation == publication &&
                cycle_not_yet_observed(scope, owner.value().cycle())
            ? bb_cycle_not_observed()
            : bb_error("program_bb_scope_stale"));
  return Result<void>::Ok();
}

Result<void> ResourceAddressedExecutionKernel::InstallAppliedProgramBlackboardContext(
    AppliedProgramBlackboardContextDTO context,
    const std::shared_ptr<IIoSignalService>& expected_service) {
  if (!context_valid(context))
    return Result<void>::Err(bb_error("program_bb_context_invalid", true));
  auto immutable = std::make_shared<const AppliedProgramBlackboardContextDTO>(std::move(context));
  std::lock_guard submission_lock(submission_mutex_);
  std::lock_guard admission_lock(*admission_mutex_);
  if (!config_.operation_admission_gate ||
      config_.operation_admission_gate->has_held_configuration_fences())
    return Result<void>::Err(bb_error("program_bb_apply_fence_held"));
  // Owner -> Kernel state is the existing validator order. The guard retains
  // actual health through commit; no journal/host I/O or Stop dispatch occurs.
  auto owner = cell_admission_owner_->AcquireProgramBlackboardAdmission();
  if (!owner.ok()) return Result<void>::Err(owner.error());
  std::lock_guard state_lock(state_mutex_);
  if (program_blackboard_runner_loss_locked(owner.value().cycle()))
    return Result<void>::Err(bb_error("program_bb_owner_not_current"));
  if (owner.value().retained_bundle_id() != immutable->retained_bundle_id || !expected_service ||
      io_signal_service_ != expected_service ||
      immutable->request_owner_epoch != config_.request_owner_epoch ||
      stop_retained_bundle_id_ != immutable->retained_bundle_id ||
      stop_graph_revision_ != immutable->graph_revision)
    return Result<void>::Err(bb_error("program_bb_context_stale"));
  if (applied_program_blackboard_context_) {
    const auto& current = *applied_program_blackboard_context_;
    if (immutable->active_publication_generation < current.active_publication_generation)
      return Result<void>::Err(bb_error("program_bb_context_stale"));
    if (immutable->active_publication_generation == current.active_publication_generation) {
      if (!same_context(*immutable, current))
        return Result<void>::Err(bb_error("program_bb_context_conflict", true));
      return Result<void>::Ok();  // Repeated same actual publication notice is inert.
    }
  }
  const auto retired = retire_program_blackboard_source_locked();
  if (!retired.ok()) return retired;
  applied_program_blackboard_context_ = std::move(immutable);
  return Result<void>::Ok();
}

Result<IoProgramBlackboardAcknowledgementDTO>
ResourceAddressedExecutionKernel::PublishProgramBlackboard(
    const IoProgramBlackboardPublicationDTO& publication) {
  auto admitted = admit_program_blackboard_frame(publication);
  // BBK-1: this frame's own evaluation (trip, Stop, release) runs before its
  // ACK, as for set_rule_active; an evaluation failure surfaces on the host's
  // next ProcessIoRuleEvidence.
  if (admitted.ok()) (void)ProcessIoRuleEvidence();
  return admitted;
}

Result<IoProgramBlackboardAcknowledgementDTO>
ResourceAddressedExecutionKernel::admit_program_blackboard_frame(
    const IoProgramBlackboardPublicationDTO& publication) {
  using Reply = Result<IoProgramBlackboardAcknowledgementDTO>;
  const auto trusted_received_at_ns = clock_.now();
  std::lock_guard submission_lock(submission_mutex_);
  std::lock_guard admission_lock(*admission_mutex_);
  if (!config_.operation_admission_gate ||
      config_.operation_admission_gate->has_held_configuration_fences())
    return Reply::Err(bb_error("program_bb_apply_fence_held"));
  auto owner = cell_admission_owner_->AcquireProgramBlackboardAdmission();
  if (!owner.ok()) {
    // The Manager's brief transition close: not admitted, retried by the
    // Runner within its existing budget once readiness is current again.
    const auto reason = owner.error().details.find("reason");
    if (reason != owner.error().details.end() &&
        reason->second == "program_bb_cycle_status_closed")
      return Reply::Err({"bb_source_not_ready",
                         "Program cycle readiness is closed for a transition",
                         {{"reason", "program_bb_cycle_status_closed"}}});
    return Reply::Err(owner.error());
  }
  std::lock_guard state_lock(state_mutex_);
  if (program_blackboard_runner_loss_locked(owner.value().cycle()))
    return Reply::Err(bb_error("program_bb_owner_not_current"));
  const auto& context = applied_program_blackboard_context_;
  if (!context || !io_signal_service_ ||
      owner.value().retained_bundle_id() != context->retained_bundle_id ||
      context->request_owner_epoch != config_.request_owner_epoch ||
      stop_retained_bundle_id_ != context->retained_bundle_id ||
      stop_graph_revision_ != context->graph_revision)
    return Reply::Err(bb_error("program_bb_source_not_installed"));
  const auto& scope = publication.scope;
  const bool current =
      scope.request_owner_epoch == context->request_owner_epoch &&
      scope.program_binding_id == context->program_binding_id &&
      scope.active_publication_generation == context->active_publication_generation &&
      program_blackboard_scope_current_locked(scope, owner.value().cycle(), *context);
  if (!current) {
    if (program_blackboard_source_ && !program_blackboard_source_retired_ &&
        program_blackboard_scope_current_locked(program_blackboard_source_->scope,
                                                owner.value().cycle(), *context) &&
        program_blackboard_source_->scope.active_publication_generation ==
            context->active_publication_generation) {
      const auto known = io_signal_service_->WasProgramBlackboardScopeRetired(scope);
      if (!known.ok()) return Reply::Err(known.error());
      if (known.value()) {
        const auto& actual = *program_blackboard_source_;
        return Reply::Ok({actual.scope.request_owner_epoch, actual.scope.program_binding_id,
                          actual.scope.cycle_generation, actual.scope.program_run_id,
                          actual.incarnation, std::nullopt,
                          actual.scope.active_publication_generation,
                          IoProgramBlackboardAcknowledgementStatus::kSuperseded});
      }
    }
    if (scope.request_owner_epoch == context->request_owner_epoch &&
        scope.program_binding_id == context->program_binding_id &&
        scope.active_publication_generation == context->active_publication_generation &&
        cycle_not_yet_observed(scope, owner.value().cycle()))
      return Reply::Err(bb_cycle_not_observed());
    return Reply::Err(bb_error("program_bb_scope_stale"));
  }
  auto received = publication;
  // Capture at this trusted receiver entry, before a blocked admission/drain
  // can restamp an old frame. External/device timestamps never grant freshness.
  received.trusted_received_at_ns = trusted_received_at_ns;
  if (!program_blackboard_source_ || program_blackboard_source_retired_ ||
      program_blackboard_source_->scope != scope) {
    if (publication.kind != IoBlackboardPublicationKind::kBaseline)
      return Reply::Err(bb_error("program_bb_baseline_required", true));
    IoProgramBlackboardSourceBindingDTO binding{scope, InterruptSourceId("program.blackboard"),
                                                source_incarnation(scope), context->required_keys,
                                                context->maximum_value_bytes};
    const auto accepted = io_signal_service_->PublishProgramBlackboardBaseline(binding,
        program_blackboard_source_ ? std::optional(program_blackboard_source_->incarnation)
                                   : std::nullopt, received);
    if (!accepted.ok()) return accepted;
    // The service owns one validated source/snapshot/registration commit. An
    // invalid replacement never retires the previous factual value store.
    program_blackboard_source_ = std::move(binding);
    program_blackboard_source_retired_ = false;
    return accepted;
  }
  return io_signal_service_->PublishProgramBlackboard(received);
}

}  // namespace manyforge::core
