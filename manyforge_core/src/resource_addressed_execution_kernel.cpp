#include "resource_addressed_execution_kernel_internal.hpp"
#include <manyforge_core/time/stuck_drain_wait.hpp>

#include <cstdio>

namespace manyforge::core {

std::string bounded_operation_journal_diagnostic_text(std::string_view value,
                                                      std::size_t maximum_bytes) {
  if (content_identity::is_utf8_nfc(value)) {
    auto size = std::min(value.size(), maximum_bytes);
    while (size < value.size() && size > 0U &&
           (static_cast<unsigned char>(value[size]) & 0xc0U) == 0x80U)
      --size;
    return std::string(value.substr(0U, size));
  }
  // Match the existing launch-output policy for otherwise unrepresentable
  // bytes. A marker identifies the representation; never split an escape.
  constexpr std::string_view marker = "escaped bytes: ";
  constexpr char hex[] = "0123456789abcdef";
  std::string result(marker.substr(0U, maximum_bytes));
  for (const auto character : value) {
    const auto byte = static_cast<unsigned char>(character);
    const std::size_t needed = byte >= 0x80U ? 4U : character == '\\' ? 2U : 1U;
    if (needed > maximum_bytes - result.size()) break;
    if (byte >= 0x80U) {
      result += "\\x";
      result += hex[byte >> 4U];
      result += hex[byte & 0x0fU];
    } else if (character == '\\') {
      result += "\\\\";
    } else {
      result += character;
    }
  }
  return result;
}

std::optional<OperationJournalWriteFaultDTO>
ResourceAddressedExecutionKernel::get_operation_journal_write_fault() const {
  std::lock_guard state_lock(state_mutex_);
  return operation_journal_write_fault_;
}

std::optional<KernelInternalFaultDTO> ResourceAddressedExecutionKernel::get_internal_fault()
    const {
  std::lock_guard state_lock(state_mutex_);
  // The alarm is raised from the first observation, so it must already name
  // the containment Stop outcome; the request is still in progress until then.
  if (internal_fault_ && !internal_fault_->stop_accepted &&
      !internal_fault_->stop_refusal_code)
    return std::nullopt;
  return internal_fault_;
}

void ResourceAddressedExecutionKernel::retain_operation_journal_write_fault_locked(
    std::string_view write_kind, const ErrorDTO& failure, const OperationStateDTO* state,
    std::optional<RequestId> request_id) {
  // Logged and carried on: a journal write fault blocks no motion (only the
  // first is kept as the runtime-fault fact).
  if (operation_journal_write_fault_) return;
  (void)std::fprintf(stderr, "manyforge_journal_write_fault kind=%.*s code=%s\n",
                     static_cast<int>(write_kind.size()), write_kind.data(),
                     failure.code.c_str());
  OperationJournalWriteFaultDTO fact{
      config_.request_owner_epoch,
      bounded_operation_journal_diagnostic_text(write_kind, 64U),
      ErrorDTO{bounded_operation_journal_diagnostic_text(failure.code, 256U),
               bounded_operation_journal_diagnostic_text(failure.message, 1024U), {}},
      std::nullopt, std::nullopt, std::move(request_id), std::nullopt, std::nullopt};
  if (state) {
    fact.operation_id = state->operation_id;
    fact.operation_revision = state->operation_revision;
    fact.program_run_id = state->program_run_id;
    if (state->participants.size() == 1U)
      fact.execution_scope = state->participants.front().execution_scope;
  }
  operation_journal_write_fault_ = std::move(fact);
}

void ResourceAddressedExecutionKernel::stamp_operation_capture_locked(ActiveOperation& operation,
                                                                      OperationStateDTO& state) {
  if (operation.last_capture_order == static_cast<std::uint64_t>(INT64_MAX)) {
    throw std::overflow_error("operation journal capture order exhausted");
  }
  for (auto& participant : state.participants) {
    if (participant.terminal_fact && !terminal(participant.state))
      participant.terminal_fact.reset();
  }
  state.journal_capture_order = ++operation.last_capture_order;
}

Result<bool> ResourceAddressedExecutionKernel::record_operation_state_locked(
    std::unique_lock<std::mutex>& state_lock, OperationStateDTO& state,
    const RuntimeScaleStatusDTO* scale, const char* reason,
    std::span<const PreparedMotionDiagnosticsDTO> diagnostics,
    std::optional<RequestId> request_id) {
  auto* operation = find_active_operation_locked(state.operation_id);
  if (operation == nullptr || !operation->ordinary_commit_in_flight)
    return reject<bool>("operation capture has no ordinary commit guard",
                        "operation_commit_guard_missing", ErrorCode::kInvariantViolation);
  if (operation->recovery_effect_in_flight || operation->coordinated_cancellation_in_flight)
    return Result<bool>::Ok(false);
  const auto prior = operation->state;
  const auto identity = operation->commit_mutex;
  const auto retained_scale =
      scale == nullptr ? std::optional<RuntimeScaleStatusDTO>{} : std::optional(*scale);
  stamp_operation_capture_locked(*operation, state);
  state_lock.unlock();
  auto written = attempt_journal_write([&] {
    return journal_.RecordOperationState(config_.request_owner_epoch, state,
                                         retained_scale ? &*retained_scale : nullptr, reason,
                                         diagnostics);
  });
  state_lock.lock();
  if (!written.ok()) {
    retain_operation_journal_write_fault_locked(reason, written.error(), &state,
                                               std::move(request_id));
    return Result<bool>::Err(written.error());
  }
  operation = find_active_operation_locked(state.operation_id);
  if (operation == nullptr || operation->commit_mutex != identity ||
      operation->state.operation_revision != prior.operation_revision ||
      operation->state.active_stop_id != prior.active_stop_id ||
      operation->state.active_stop_generation != prior.active_stop_generation ||
      operation->state.journal_capture_order != prior.journal_capture_order)
    return Result<bool>::Ok(false);
  // Progress-only samples do not need durable writes. Do not overwrite a
  // newer such sample on an unchanged participant when this capture commits.
  for (std::size_t index = 0; index < state.participants.size(); ++index) {
    auto& row = state.participants[index];
    if (row.state == prior.participants[index].state &&
        row.progress == prior.participants[index].progress)
      row.progress = operation->state.participants[index].progress;
  }
  state.updated_at_ns = std::max(state.updated_at_ns, operation->state.updated_at_ns);
  return Result<bool>::Ok(true);
}


ResourceAddressedExecutionKernel::ActiveOperation*
ResourceAddressedExecutionKernel::find_active_operation_locked(
    const OperationId& operation_id) noexcept {
  const auto found = active_operations_.find(operation_id);
  return found == active_operations_.end() ? nullptr : found->second.get();
}

const ResourceAddressedExecutionKernel::ActiveOperation*
ResourceAddressedExecutionKernel::find_active_operation_locked(
    const OperationId& operation_id) const noexcept {
  const auto found = active_operations_.find(operation_id);
  return found == active_operations_.end() ? nullptr : found->second.get();
}

std::vector<OperationId> ResourceAddressedExecutionKernel::durable_operation_retention_ids_locked()
    const {
  std::vector<ResourceId> unresolved_resources;
  for (const auto& slot : resource_slots_->snapshot()) {
    if (slot.state == ResourceSlotState::kUnreconciledExternalOrOrphanGoal) {
      unresolved_resources.push_back(slot.resource_id);
    }
  }
  std::sort(unresolved_resources.begin(), unresolved_resources.end());

  std::vector<OperationId> retained;
  retained.reserve(active_operations_.size() + historical_operations_.size());
  for (const auto& [operation_id, operation] : active_operations_) {
    retained.push_back(operation_id);
    for (const auto* slot : {&operation->pending_motion_takeover, &operation->next_motion_takeover})
      if (!slot->empty()) retained.push_back(slot->key());
  }
  if (manual_operation_claim_.has_value()) {
    retained.emplace_back(manual_operation_claim_->intent.owner_transaction_id);
  }
  for (const auto& operation : historical_operations_) {
    const bool replaced_before_settlement = std::any_of(
        operation.participants.begin(), operation.participants.end(), [](const auto& participant) {
          return participant.terminal_error.has_value() &&
                 participant.terminal_error->code == "execution_host_replaced";
        });
    const bool references_unresolved_resource = std::any_of(
        operation.participants.begin(), operation.participants.end(), [&](const auto& participant) {
          return std::any_of(participant.resources.begin(), participant.resources.end(),
                             [&](const auto& resource) {
                               return std::binary_search(unresolved_resources.begin(),
                                                         unresolved_resources.end(), resource);
                             });
        });
    if (replaced_before_settlement && references_unresolved_resource) {
      retained.push_back(operation.operation_id);
    }
  }
  std::sort(retained.begin(), retained.end());
  retained.erase(std::unique(retained.begin(), retained.end()), retained.end());
  return retained;
}

bool ResourceAddressedExecutionKernel::evict_oldest_historical_operation_locked(
    const std::span<const OperationId> protected_operation_ids) {
  const auto compactable = std::find_if(
      historical_operations_.begin(), historical_operations_.end(), [&](const auto& operation) {
        return !std::binary_search(protected_operation_ids.begin(), protected_operation_ids.end(),
                                   operation.operation_id);
      });
  if (compactable == historical_operations_.end()) {
    return false;
  }
  const auto retired_id = compactable->operation_id;
  historical_operations_.erase(compactable);
  std::erase_if(replay_ledger_, [&](const ReplayEntry& entry) {
    return entry.accepted.operation_id == retired_id;
  });
  std::erase_if(control_replay_ledger_, [&](const ControlReplayEntry& entry) {
    return entry.accepted.operation_id == retired_id;
  });
  std::erase_if(runtime_scale_replay_ledger_, [&](const RuntimeScaleReplayEntry& entry) {
    return entry.accepted.operation_id == retired_id;
  });
  std::erase_if(operation_recovery_replay_ledger_, [&](const OperationRecoveryReplayEntry& entry) {
    return entry.operation_id == retired_id;
  });
  return true;
}

std::size_t ResourceAddressedExecutionKernel::resident_operation_count_locked() const noexcept {
  auto count = active_operations_.size();
  for (const auto& [id, operation] : active_operations_) {
    static_cast<void>(id);
    count += !operation->pending_motion_takeover.empty();
    count += !operation->next_motion_takeover.empty();
  }
  return count;
}

bool ResourceAddressedExecutionKernel::make_operation_admission_capacity_locked() {
  const auto protected_operation_ids = durable_operation_retention_ids_locked();
  while (!historical_operations_.empty() &&
         resident_operation_count_locked() + historical_operations_.size() >=
             config_.request_replay_capacity) {
    if (!evict_oldest_historical_operation_locked(protected_operation_ids)) {
      return false;
    }
  }
  return resident_operation_count_locked() + historical_operations_.size() <
         config_.request_replay_capacity;
}

void ResourceAddressedExecutionKernel::retain_historical_operation_locked(OperationStateDTO state) {
  // Terminal operation detail is diagnostic history, not execution
  // authority. Keep it and the latest state-conditioned correlations bounded
  // together. Active/uncertain operations remain in active_operations_ and
  // are therefore never selected for this compaction.
  const auto protected_operation_ids = durable_operation_retention_ids_locked();
  while (!historical_operations_.empty() &&
         resident_operation_count_locked() + historical_operations_.size() >
             config_.request_replay_capacity) {
    if (!evict_oldest_historical_operation_locked(protected_operation_ids)) {
      break;
    }
  }
  historical_operations_.push_back(std::move(state));
}


ResourceAddressedExecutionKernel::ResourceAddressedExecutionKernel(
    std::vector<RuntimeParticipantExecutionRegistration> registrations, IClock& clock,
    IScheduler& scheduler, IOperationJournal& journal,
    ResourceAddressedExecutionKernelConfig config)
    : registry_(extract_bindings(registrations)),
      admission_mutex_(config.operation_admission_gate != nullptr
                           ? config.operation_admission_gate->shared_admission_mutex()
                           : std::make_shared<std::recursive_mutex>()),
      resource_slots_(std::make_shared<ResourceSlotTable>(
          registrations.empty() ? config.empty_runtime_resource_inventory
                                : registry_.resource_inventory(),
          extract_published_active_control_configurations(registrations), admission_mutex_)),
      registrations_(std::move(registrations)),
      clock_(clock),
      scheduler_(scheduler),
      journal_(journal),
      config_(std::move(config)),
      engine_event_ingress_(std::make_shared<EngineEventIngress>()),
      emergency_stop_reserve_(config_.emergency_stop_correlation_capacity),
      stop_transactions_(config_.emergency_stop_correlation_capacity + 1U),
      stop_correlations_(config_.emergency_stop_correlation_capacity + 1U),
      stop_projection_jobs_(config_.stop_forensic_projection_capacity == 0U
                                ? 2U * (config_.emergency_stop_correlation_capacity + 1U) +
                                      kMaximumResidentStopTerminalRecords
                                : config_.stop_forensic_projection_capacity),
      stop_projection_generations_(stop_projection_jobs_.size()) {
  if (!config_.empty_runtime_resource_inventory.empty() &&
      (!registrations_.empty() || !config_.empty_runtime_retained_bundle_id.has_value() ||
       !config_.empty_runtime_graph_revision.has_value() ||
       !std::is_sorted(config_.empty_runtime_resource_inventory.begin(),
                       config_.empty_runtime_resource_inventory.end()) ||
       std::adjacent_find(config_.empty_runtime_resource_inventory.begin(),
                          config_.empty_runtime_resource_inventory.end()) !=
           config_.empty_runtime_resource_inventory.end())) {
    throw std::invalid_argument(
        "bootstrap resource inventory lacks one exact empty-runtime product");
  }
  if (config_.operator_alarm_attention.has_value() &&
      (config_.operator_alarm_attention->maximum_snapshot_age_ns <= 0 ||
       config_.operator_alarm_attention->host_boot_id.empty())) {
    throw std::invalid_argument(
        "operator-alarm attention policy needs a positive age bound and the host boot id");
  }
  if (config_.request_owner_epoch.empty() || config_.cell_id.empty() ||
      config_.request_replay_capacity == 0U ||
      config_.request_replay_capacity > kMaxI6OperationReplayEntries ||
      config_.cell_admission_claim_capacity == 0U ||
      config_.cell_admission_claim_capacity > kMaximumCellAdmissionClaims) {
    throw std::invalid_argument(
        "addressed Kernel request-owner epoch, cell identity, or replay bound is invalid");
  }
  if (config_.operation_admission_gate != nullptr &&
      !resource_slots_->shares_admission_mutex(
          config_.operation_admission_gate->shared_admission_mutex())) {
    throw std::invalid_argument(
        "addressed Kernel admission gate does not share its resource mutex");
  }
  const auto maximum_projection_capacity =
      2U * (config_.emergency_stop_correlation_capacity + 1U) + kMaximumResidentStopTerminalRecords;
  if (stop_projection_jobs_.empty() || stop_projection_jobs_.size() > maximum_projection_capacity) {
    throw std::invalid_argument("addressed Kernel forensic projection bound is invalid");
  }
  auto stop_facts = derive_stop_static_facts(registrations_, config_);
  stop_retained_bundle_id_ = std::move(stop_facts.retained_bundle_id);
  stop_graph_revision_ = std::move(stop_facts.graph_revision);
  stop_component_scope_ = std::move(stop_facts.components);
  for (const auto& configuration : extract_active_control_configurations(registrations_)) {
    if (!domain_has_complete_stop_readiness(registrations_, configuration.control_domain_id)) {
      runtime_route_configuration_publication_required_.insert(configuration.control_domain_id);
    }
  }
  replay_ledger_.reserve(config_.request_replay_capacity);
  control_replay_ledger_.reserve(config_.request_replay_capacity);
  stop_replay_ledger_.reserve(config_.request_replay_capacity);
  runtime_scale_replay_ledger_.reserve(config_.request_replay_capacity);
  runtime_scale_control_replay_ledger_.reserve(config_.request_replay_capacity);
  runtime_scale_control_settings_.reserve(registrations_.size());
  operation_recovery_replay_ledger_.reserve(config_.request_replay_capacity);
  program_run_stop_fences_.reserve(config_.request_replay_capacity);
  global_io_rule_projection_queue_.reserve(config_.request_replay_capacity);
  stop_terminal_history_.reserve(kMaximumResidentStopTerminalRecords);
  for (std::size_t index = 0U; index < stop_transactions_.size(); ++index) {
    stop_transactions_[index] = std::make_unique<StopTransaction>();
  }
  for (std::size_t index = 0U; index < stop_projection_jobs_.size(); ++index) {
    stop_projection_jobs_[index] = std::make_unique<StopProjectionJob>();
  }
  stop_projection_worker_ =
      std::make_unique<StopProjectionWorker>(stop_projection_jobs_.size(), *this);
  stop_control_executor_ = std::make_unique<BoundedStopControlExecutor>(
      emergency_stop_reserve_.correlation_capacity() + 1U, clock_,
      static_cast<IStopControlWorkReceiver&>(*this));
  if (!stop_control_executor_->prequalified_ready() ||
      stop_control_executor_->capacity() != emergency_stop_reserve_.correlation_capacity() + 1U) {
    throw std::runtime_error("StopControl executor failed its reserve-capacity admission join");
  }
  const auto initially_qualified_participants = qualified_execution_engine_count(registrations_);
  prequalified_max_live_stop_participants_ =
      config_.prequalified_max_live_stop_participants == 0U
          ? std::max<std::size_t>(1U, initially_qualified_participants)
          : config_.prequalified_max_live_stop_participants;
  if (initially_qualified_participants > prequalified_max_live_stop_participants_ ||
      prequalified_max_live_stop_participants_ >
          kMaximumPrequalifiedStopStageAttemptLanes / kMaxStopEscalationStages) {
    throw std::invalid_argument(
        "Stop-qualified runtime participants exceed the prequalified attempt-isolation bound");
  }
  stop_stage_attempt_executor_ = std::make_unique<BoundedStopStageAttemptExecutor>(
      prequalified_max_live_stop_participants_ * kMaxStopEscalationStages,
      prequalified_max_live_stop_participants_,
      static_cast<IStopStageAttemptCompletionReceiver&>(*this));
  if (!stop_stage_attempt_executor_->prequalified_ready() ||
      stop_stage_attempt_executor_->prequalified_max_live_participants() !=
          prequalified_max_live_stop_participants_) {
    throw std::runtime_error("Stop-stage attempt isolation failed its admission prequalification");
  }
  auto operation_control_engines = distinct_execution_engines(registrations_);
  if (!operation_control_engines.empty()) {
    operation_control_executor_ = std::make_unique<BoundedOperationControlExecutor>(
        std::move(operation_control_engines),
        static_cast<IOperationControlEffectCompletionReceiver&>(*this));
    if (!operation_control_executor_->prequalified_ready()) {
      throw std::runtime_error("operation-control isolation failed its admission prequalification");
    }
  }

  std::vector<ResourceId> recovery_inventory;
  for (const auto& slot : resource_slots_->snapshot())
    recovery_inventory.push_back(slot.resource_id);
  auto recovery = journal_.BeginOwnerEpoch(config_.request_owner_epoch, clock_.now(),
                                           config_.request_replay_capacity, recovery_inventory);
  if (!recovery.ok()) {
    throw std::runtime_error("addressed Kernel durable owner admission failed: " +
                             recovery.error().code + ": " + recovery.error().message);
  }
  historical_operations_ = std::move(recovery.value().historical_operations);
  cell_admission_owner_ = std::make_unique<KernelCellAdmissionOwner>(
      config_.request_owner_epoch,
      std::min(config_.request_replay_capacity, config_.cell_admission_claim_capacity), clock_,
      journal_, std::move(recovery.value().recovered_cell_admission_claims),
      recovery.value().cell_admission_generation_high_water,
      config_.cell_admission_intervention_validator,
      [this](const CellAdmissionClaimDTO& claim, const CellAdmissionClaimReleaseDTO& release) {
        if (claim.intent.claim_kind == CellAdmissionClaimKindDTO::kManualOperation) {
          return validate_manual_operation_settlement(claim, release);
        }
        return config_.cell_admission_settlement_validator
                   ? config_.cell_admission_settlement_validator(claim, release)
                   : reject<void>("settlement owner is unavailable",
                                  "settlement_owner_reader_unavailable");
      },
      [this](const std::vector<CellAdmissionClaimDTO>& held,
             const std::vector<CellAdmissionCurrentOwnerEpochDTO>& current,
             const CellAdmissionReconcileDTO& request) {
        const auto predecessor = std::find_if(current.begin(), current.end(), [](const auto& row) {
          return row.owner_role == CellAdmissionOwnerRoleDTO::kManualOperationOwner;
        });
        const auto successor = std::find_if(
            request.current_owner_epochs.begin(), request.current_owner_epochs.end(),
            [](const auto& row) {
              return row.owner_role == CellAdmissionOwnerRoleDTO::kManualOperationOwner;
            });
        if (predecessor != current.end() &&
            (successor == request.current_owner_epochs.end() || *successor != *predecessor)) {
          return validate_manual_owner_reconciliation(held, current, request);
        }
        return config_.cell_admission_reconcile_validator
                   ? config_.cell_admission_reconcile_validator(held, current, request)
                   : reject<CellAdmissionReconcileValidationDTO>(
                         "reconciliation owner is unavailable",
                         "reconciliation_owner_reader_unavailable");
      },
      std::move(recovery.value().recovered_cell_admission_membership),
      std::move(recovery.value().recovered_cell_admission_current_owners),
      config_.manager_liveness_required_set, recovery.value().cell_claim_generation_high_water,
      [this](const ProgramNodeAdmissionEvidenceDTO& evidence) {
        return validate_program_node_claim(evidence);
      },
      config_.cell_admission_program_reset_status_source,
      [this](std::shared_ptr<IIoSignalService>* affected_service) {
        std::lock_guard state_lock(state_mutex_);
        return retire_program_blackboard_source_locked(affected_service);
      });
  refresh_execution_claims();  // construction is single-threaded
  if (recovery.value().recovered_stops.size() > kMaximumResidentStopTerminalRecords) {
    throw std::runtime_error("recovered Stop terminal history exceeds its fixed bound");
  }
  for (const auto& recovered : recovery.value().recovered_stops) {
    cell_stop_generation_ = std::max(cell_stop_generation_, recovered.accepted.stop_generation);
    if (!recovered.terminal_certificate.has_value()) {
      // An accepted Stop with no projected terminal certificate is never
      // reconstructed or redispatched. Motion admission waits until every
      // component reports healthy and idle (the armed cell latch, R2).
      motion_admission_unreconciled_ = true;
      continue;
    }
    const auto& certificate = *recovered.terminal_certificate;
    const auto terminal_state = [&] {
      switch (certificate.result) {
        case StopCertificateResultDTO::kSucceededNominal:
        case StopCertificateResultDTO::kSucceededEscalated:
          return StopStatusStateDTO::kSucceeded;
        case StopCertificateResultDTO::kPartial:
          return StopStatusStateDTO::kPartial;
        case StopCertificateResultDTO::kFailedUncontained:
          return StopStatusStateDTO::kFailed;
      }
      return StopStatusStateDTO::kFailed;
    }();
    const StopCertificateRefDTO reference{certificate.stop_certificate_id,
                                          certificate.stop_certificate_revision,
                                          certificate.stop_id, certificate.stop_generation};
    StopStatusDTO status{
        recovered.accepted,
        terminal_state,
        certificate.scope_entries,
        {},
        certificate.attempted_stage_results,
        certificate.evidence_items,
        certificate.unsettled,
        certificate.failed,
        StopDurableProjectionStatusDTO{StopDurableProjectionStateDTO::kRecorded, std::nullopt,
                                       std::nullopt},
        StopDurableProjectionStatusDTO{StopDurableProjectionStateDTO::kRecorded, std::nullopt,
                                       std::nullopt},
        reference};
    if (stop_terminal_projection_field_error(status, certificate).has_value()) {
      throw std::runtime_error("recovered Stop terminal projection is invalid");
    }
    stop_terminal_history_.push_back(std::make_unique<StopTerminalRecord>(
        StopTerminalRecord{std::move(status), certificate, std::nullopt}));
  }
  if (recovery.value().replaced_unclosed_owner_epoch.has_value()) {
    motion_admission_unreconciled_ = true;
  }
  if (!recovery.value().unreconciled_resources.empty()) {
    motion_admission_unreconciled_ = true;
    recovered_orphan_resources_ = recovery.value().unreconciled_resources;
    std::sort(recovered_orphan_resources_.begin(), recovered_orphan_resources_.end());
    auto marked = resource_slots_->mark_unreconciled_external_or_orphan_goal(
        recovery.value().unreconciled_resources);
    if (!marked.ok()) {
      throw std::runtime_error("addressed Kernel durable orphan recovery failed: " +
                               marked.error().code + ": " + marked.error().message);
    }
  }

  try {
    for (const auto& registration : registrations_) {
      if (std::any_of(engine_subscriptions_.begin(), engine_subscriptions_.end(),
                      [&](const EngineSubscription& item) {
                        return item.engine == registration.engine;
                      })) {
        continue;
      }
      auto* engine = registration.engine;
      auto ingress = engine_event_ingress_;
      const auto handle = engine->subscribe_events(
          [ingress = std::move(ingress), engine](const KernelEvent& event) {
            auto entered = ingress->Enter();
            if (auto* owner = entered.owner()) owner->on_engine_event(engine, event);
          });
      engine_subscriptions_.push_back(EngineSubscription{engine, handle});
    }
    // Opening is the final construction step. A callback captured while the
    // constructor was still assembling subscriptions observes closed ingress
    // rather than a partially constructed Kernel.
    engine_event_ingress_->Open(*this);
  } catch (...) {
    for (const auto& subscription : engine_subscriptions_) {
      subscription.engine->unsubscribe_events(subscription.handle);
    }
    engine_subscriptions_.clear();
    throw;
  }
}

ResourceAddressedExecutionKernel::~ResourceAddressedExecutionKernel() {
  // Engine callbacks are the outermost asynchronous ingress. Closing the
  // guard first prevents a callback already copied by a publisher from
  // acquiring the Kernel. Unsubscribe removes future snapshots, and Drain
  // waits for callbacks that entered before Close while all state and worker
  // lanes they may reach are still alive and open.
  engine_event_ingress_->Close();
  for (const auto& subscription : engine_subscriptions_) {
    subscription.engine->unsubscribe_events(subscription.handle);
  }
  engine_subscriptions_.clear();
  engine_event_ingress_->Drain();
  {
    std::lock_guard state_lock(state_mutex_);
    if (program_cycle_claim_run_)
      retire_program_io_watches_locked(program_cycle_claim_run_->program_run_id);
    retire_all_io_rules_locked();
    static_cast<void>(retire_program_blackboard_source_locked());
  }

  // With participant events drained, each lane can close its own ingress and
  // settle all causally accepted work before its worker object is retired.
  if (operation_control_executor_ != nullptr) {
    operation_control_executor_->CloseIngress();
    WaitForDrain("kernel.operation_control_executor", std::chrono::seconds(5),
                 [&](std::chrono::nanoseconds bound) {
                   return operation_control_executor_->DrainFor(bound);
                 });
  }
  if (stop_stage_attempt_executor_ != nullptr) {
    stop_stage_attempt_executor_->CloseIngress();
    WaitForDrain("kernel.stop_stage_attempt_executor", std::chrono::seconds(5),
                 [&](std::chrono::nanoseconds bound) {
                   return stop_stage_attempt_executor_->DrainFor(bound);
                 });
  }
  if (stop_control_executor_ != nullptr) {
    // Attempt completion callbacks remain allowed to rearm/publish their
    // owning StopControl slot until every isolated provider call has drained.
    // Drain those causal wakeups before closing StopControl ingress.
    WaitForDrain("kernel.stop_control_executor", std::chrono::seconds(5),
                 [&](std::chrono::nanoseconds bound) {
                   return stop_control_executor_->DrainFor(bound);
                 });
    stop_control_executor_->CloseIngress();
  }
  if (stop_projection_worker_ != nullptr) {
    // Accepted Stop projections settle here, while the journal, clock,
    // scheduler and every participant engine are still alive.
    stop_projection_worker_->Close();
    WaitForDrain("kernel.stop_projection_worker", std::chrono::seconds(5),
                 [&](std::chrono::nanoseconds bound) {
                   return stop_projection_worker_->DrainFor(bound);
                 });
  }
  // Only now may the drained lanes themselves be destroyed. Nothing can reach
  // them afterwards, so a lane cannot publish into the journal below.
  operation_control_executor_.reset();
  stop_control_executor_.reset();
  stop_stage_attempt_executor_.reset();
  stop_projection_worker_.reset();
  // The durable journal is the final authority for clean-close eligibility.
  // At this point every Kernel-owned asynchronous lane has drained, so it can
  // atomically prove that all operations and Stop projections are terminal.
  // A rejection or storage failure deliberately leaves the epoch unclosed;
  // the next owner then performs conservative complete-inventory recovery.
  bool orphan_reconciliation_complete = false;
  {
    std::lock_guard state_lock(state_mutex_);
    orphan_reconciliation_complete = !motion_admission_unreconciled_;
  }
  if (orphan_reconciliation_complete) {
    try {
      static_cast<void>(journal_.CloseOwnerEpoch(config_.request_owner_epoch, clock_.now()));
    } catch (...) {
    }
  }
}

}  // namespace manyforge::core
