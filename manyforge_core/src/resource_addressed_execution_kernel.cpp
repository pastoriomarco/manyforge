#include "resource_addressed_execution_kernel_internal.hpp"
#include <manyforge_core/execution_event_change_signal.hpp>
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
  // Its containment Halt (S11) is never refused, so the fault is published
  // as soon as it is recorded.
  return internal_fault_;
}

bool ResourceAddressedExecutionKernel::retain_operation_journal_write_fault_fact_locked(
    std::string_view write_kind, const ErrorDTO& failure, const OperationStateDTO* state,
    std::optional<RequestKey> request_key) {
  // The first record not written of a loss episode is the alarm's fact; it
  // blocks no motion and clears when the episode ends.
  if (operation_journal_write_fault_) return false;
  OperationJournalWriteFaultDTO fact{
      bounded_operation_journal_diagnostic_text(write_kind, 64U),
      ErrorDTO{bounded_operation_journal_diagnostic_text(failure.code, 256U),
               bounded_operation_journal_diagnostic_text(failure.message, 1024U), {}},
      std::nullopt, std::nullopt, std::move(request_key), std::nullopt, std::nullopt, false};
  if (state) {
    fact.operation_id = state->operation_id;
    fact.operation_revision = state->operation_revision;
    fact.program_run_id = state->program_run_id;
    if (state->participants.size() == 1U)
      fact.execution_scope = state->participants.front().execution_scope;
  }
  operation_journal_write_fault_ = std::move(fact);
  return true;
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
  // Every stamped change, on any lane; its post or section owes the signal.
  note_operation_change_locked(false);
}

void ResourceAddressedExecutionKernel::note_operation_change_locked(const bool signal) noexcept {
  ++operation_change_;
  if (signal) state_mutex_.owe_change_signal();
}

Result<bool> ResourceAddressedExecutionKernel::record_operation_state_locked(
    OperationStateDTO& state, const RuntimeScaleStatusDTO* scale, const char* reason,
    std::span<const PreparedMotionDiagnosticsDTO> diagnostics,
    std::optional<RequestKey> request_key) {
  auto* operation = find_active_operation_locked(state.operation_id);
  if (operation == nullptr || !operation->ordinary_commit_in_flight)
    return reject<bool>("operation capture has no ordinary commit guard",
                        "operation_commit_guard_missing", ErrorCode::kInvariantViolation);
  if (operation->recovery_effect_in_flight || operation->coordinated_cancellation_in_flight)
    return Result<bool>::Ok(false);
  // The lock stays held: the capture is stamped, posted and then applied by
  // the caller in this one section.
  stamp_operation_capture_locked(*operation, state);
  // Ordinary rows carry the participants' `stop` objects too (C4).
  fill_participant_stop_observations(operation->participants, state);
  post_operation_state_locked(
      *operation,
      JournalOperationState{state,
                            scale == nullptr ? std::optional<RuntimeScaleStatusDTO>{}
                                             : std::optional(*scale),
                            reason,
                            {diagnostics.begin(), diagnostics.end()}},
      reason, std::move(request_key));
  state.updated_at_ns = std::max(state.updated_at_ns, operation->state.updated_at_ns);
  return Result<bool>::Ok(true);
}

namespace {
const OperationStateDTO* journal_record_state(const JournalRecord& record) noexcept {
  if (const auto* accepted = std::get_if<JournalOperationAccepted>(&record.payload))
    return &accepted->state;
  if (const auto* control = std::get_if<JournalControlAccepted>(&record.payload))
    return &control->state;
  if (const auto* observed = std::get_if<JournalOperationState>(&record.payload))
    return &observed->state;
  if (const auto* scale = std::get_if<JournalRuntimeScaleAccepted>(&record.payload))
    return &scale->state;
  return nullptr;
}
}  // namespace

void ResourceAddressedExecutionKernel::post_journal_record_locked(JournalRecord record) {
  // An operation's record (accepted, control accepted, speed accepted) is
  // applied in this section whether or not its row is queued: the stream
  // reads it from memory.
  if (journal_record_state(record) != nullptr) note_operation_change_locked();
  if (journal_queue_.size() >= kJournalQueueCapacity) {
    ++journal_records_dropped_;
    on_journal_queue_full_locked(record);
    return;
  }
  journal_queue_.push_back(std::move(record));
  journal_posted_count_.fetch_add(1U, std::memory_order_release);
  wake_journal_writer();
}

void ResourceAddressedExecutionKernel::post_stop_journal_record_locked(
    JournalRecord record) noexcept {
  const auto capacity = std::holds_alternative<JournalStopEventRow>(record.payload)
                            ? kJournalQueueCapacity + kJournalQueueStopReserve
                            : kJournalQueueCapacity;
  if (journal_queue_.size() >= capacity) {
    ++journal_records_dropped_;
    on_journal_queue_full_locked(record);
    return;
  }
  journal_queue_.push_back(std::move(record));  // reserved: never reallocates
  journal_posted_count_.fetch_add(1U, std::memory_order_release);
}

void ResourceAddressedExecutionKernel::post_operation_state_locked(
    ActiveOperation& operation, JournalOperationState state, const char* write_kind,
    std::optional<RequestKey> request_key, const bool stop_lane) {
  // A record with prepared diagnostics becomes an `operation_prepared` row,
  // which the journal keeps: it is never superseded.
  const bool latest_wins = state.prepared_motion_diagnostics.empty();
  // The state is applied in this section whether or not its row is queued.
  note_operation_change_locked(!stop_lane);
  JournalRecord record{std::move(state), write_kind, std::move(request_key), stop_lane, false};
  if (journal_queue_.size() >= kJournalQueueCapacity) {
    ++journal_records_dropped_;
    on_journal_queue_full_locked(record);
    return;
  }
  if (latest_wins) {
    if (operation.journal_state_generation == journal_queue_generation_)
      journal_queue_[operation.journal_state_index].skipped = true;
    operation.journal_state_generation = journal_queue_generation_;
    operation.journal_state_index = journal_queue_.size();
  }
  journal_queue_.push_back(std::move(record));
  journal_posted_count_.fetch_add(1U, std::memory_order_release);
  if (!stop_lane) wake_journal_writer();
}

void ResourceAddressedExecutionKernel::post_journal_records(std::span<JournalRecord> records) {
  {
    std::lock_guard state_lock(state_mutex_);
    for (auto& record : records) {
      if (journal_queue_.size() >= kJournalQueueCapacity) {
        ++journal_records_dropped_;
        on_journal_queue_full_locked(record);
        continue;
      }
      journal_queue_.push_back(std::move(record));
      journal_posted_count_.fetch_add(1U, std::memory_order_release);
    }
  }
  wake_journal_writer();
}

void ResourceAddressedExecutionKernel::post_active_tcp_change(ActiveTcpChangeDTO change) {
  JournalRecord record{std::move(change), "active_tcp_changed"};
  post_journal_records(std::span<JournalRecord>(&record, 1U));
  // As the row's own commit did when it was written directly: the stream's
  // `state` (the robot's Active TCP baseline) is read again at once.
  execution_event_change_signal().Notify();
}

void ResourceAddressedExecutionKernel::wake_journal_writer() noexcept {
  journal_writer_->Wake();
}

void ResourceAddressedExecutionKernel::on_journal_queue_full_locked(
    const JournalRecord& record) noexcept {
  note_journal_records_lost_locked(record, 1U, journal_records_lost_error());
  // OD-215: the level the Headless disk check reads (the fact above names
  // only the episode's first cause). Set here, it stays until a batch leaves
  // the queue written empty.
  operation_journal_write_fault_->queue_full = true;
}


void ResourceAddressedExecutionKernel::note_journal_records_lost_locked(
    const JournalRecord& first, const std::uint64_t count, const ErrorDTO& error) {
  journal_records_not_written_ += count;
  const std::string_view write_kind = first.write_kind ? first.write_kind : "";
  if (retain_operation_journal_write_fault_fact_locked(write_kind, error,
                                                       journal_record_state(first),
                                                       first.fault_request_key))
    journal_loss_line_owed_ = true;
}

bool ResourceAddressedExecutionKernel::write_journal_batch(
    std::vector<JournalRecord>& taken) noexcept {
  std::uint64_t dropped = 0U;
  std::uint64_t posted_through = 0U;
  {
    std::lock_guard state_lock(state_mutex_);
    if (journal_queue_.empty() && journal_records_dropped_ == 0U) return false;
    journal_queue_.swap(taken);
    ++journal_queue_generation_;
    dropped = std::exchange(journal_records_dropped_, 0U);
    posted_through = journal_posted_count_.load(std::memory_order_relaxed);
  }
  dropped += copy_journal_stop_rows(taken);
  JournalBatchResult result;
  const auto fail_all = [&](ErrorDTO failure) {
    result.failures.clear();
    for (std::size_t index = 0U; index < taken.size(); ++index)
      if (!taken[index].skipped) result.failures.push_back({index, failure});
  };
  try {
    result = journal_.WriteBatch(taken, dropped);
  } catch (const std::exception& exception) {
    fail_all(error("operation_journal_exception", exception.what(), "operation_journal_exception"));
  } catch (...) {
    fail_all(error("operation_journal_exception", "journal write threw",
                   "operation_journal_exception"));
  }
  std::optional<OperationJournalWriteFaultDTO> episode_started;
  bool episode_ended = false;
  bool queue_written_empty = false;
  {
    std::lock_guard state_lock(state_mutex_);
    if (!result.failures.empty())
      note_journal_records_lost_locked(taken[result.failures.front().index],
                                       result.failures.size(),
                                       result.failures.front().error);
    if (std::exchange(journal_loss_line_owed_, false))
      episode_started = operation_journal_write_fault_;
    // A loss episode ends at a later batch that writes everything it took and
    // leaves the queue empty: the alarm clears.
    episode_ended = !episode_started && operation_journal_write_fault_.has_value() &&
                    result.failures.empty() && journal_queue_.empty() &&
                    journal_records_dropped_ == 0U;
    if (episode_ended) operation_journal_write_fault_.reset();
    // OD-215: "queue full" ends once a batch, written or not, leaves the
    // queue empty with no drop pending (a store failing for another reason
    // cannot hold the disk check's stop).
    queue_written_empty = !episode_ended && operation_journal_write_fault_.has_value() &&
                          operation_journal_write_fault_->queue_full &&
                          journal_queue_.empty() && journal_records_dropped_ == 0U;
    if (queue_written_empty) operation_journal_write_fault_->queue_full = false;
  }
  if (episode_started)
    (void)std::fprintf(stderr, "manyforge_journal_write_fault kind=%s code=%s\n",
                       episode_started->write_kind.c_str(), episode_started->error.code.c_str());
  // The `state` stream message carries the alarm's fact: wake its readers.
  if (episode_started || episode_ended || queue_written_empty)
    execution_event_change_signal().Notify();
  taken.clear();
  journal_writer_->MarkWritten(posted_through);
  return true;
}

void ResourceAddressedExecutionKernel::report_journal_records_not_written(
    const std::uint64_t undrained) noexcept {
  std::uint64_t not_written = 0U;
  {
    std::lock_guard state_lock(state_mutex_);
    not_written = journal_records_not_written_;
  }
  if (not_written + undrained == 0U) return;
  (void)std::fprintf(stderr,
                     "manyforge_fault code=operation_journal_records_lost not_written=%llu "
                     "undrained=%llu\n",
                     static_cast<unsigned long long>(not_written),
                     static_cast<unsigned long long>(undrained));
}


ResourceAddressedExecutionKernel::ActiveOperation*
ResourceAddressedExecutionKernel::find_active_operation_locked(
    const OperationNumber& operation_id) noexcept {
  const auto found = active_operations_.find(operation_id);
  return found == active_operations_.end() ? nullptr : found->second.get();
}

const ResourceAddressedExecutionKernel::ActiveOperation*
ResourceAddressedExecutionKernel::find_active_operation_locked(
    const OperationNumber& operation_id) const noexcept {
  const auto found = active_operations_.find(operation_id);
  return found == active_operations_.end() ? nullptr : found->second.get();
}

std::vector<OperationNumber> ResourceAddressedExecutionKernel::durable_operation_retention_ids_locked()
    const {
  std::vector<OperationNumber> retained;
  retained.reserve(3U * active_operations_.size() + 1U);
  for (const auto& [operation_id, operation] : active_operations_) {
    retained.push_back(operation_id);
    for (const auto* slot : {&operation->pending_motion_takeover, &operation->next_motion_takeover})
      if (!slot->empty()) retained.push_back(slot->key());
  }
  if (manual_operation_claim_.has_value()) {
    if (const auto operation_id =
            manual_claim_operation(manual_operation_claim_->intent.owner_transaction_id))
      retained.push_back(*operation_id);
  }
  std::sort(retained.begin(), retained.end());
  retained.erase(std::unique(retained.begin(), retained.end()), retained.end());
  return retained;
}

bool ResourceAddressedExecutionKernel::evict_oldest_historical_operation_locked(
    const std::span<const OperationNumber> protected_operation_ids) {
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
  replay_ledger_.erase_if([&](const ReplayEntry& entry) {
    return entry.accepted.operation_id == retired_id;
  });
  control_replay_ledger_.erase_if([&](const ControlReplayEntry& entry) {
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
    IOperationJournal& journal,
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
      journal_(journal),
      config_(std::move(config)),
      engine_event_ingress_(std::make_shared<EngineEventIngress>()) {
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
  if (config_.application_start_id.value == 0U || config_.cell_id.empty() ||
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
  auto stop_facts = derive_stop_static_facts(registrations_, config_);
  stop_retained_bundle_id_ = std::move(stop_facts.retained_bundle_id);
  stop_graph_revision_ = std::move(stop_facts.graph_revision);
  stop_component_scope_ = std::move(stop_facts.components);
  for (const auto& configuration : extract_active_control_configurations(registrations_)) {
    if (!domain_has_complete_stop_readiness(registrations_, configuration.control_domain_id)) {
      runtime_route_configuration_publication_required_.insert(configuration.control_domain_id);
    }
  }
  runtime_scale_control_settings_.reserve(registrations_.size());
  operation_recovery_replay_ledger_.reserve(config_.request_replay_capacity);
  // Slots and index nodes for every bounded ledger, allocated once here.
  replay_ledger_.reserve(config_.request_replay_capacity);
  control_replay_ledger_.reserve(config_.request_replay_capacity);
  runtime_scale_control_replay_ledger_.reserve(config_.request_replay_capacity);
  historical_operations_.reserve(config_.request_replay_capacity);
  journal_queue_.reserve(kJournalQueueCapacity + kJournalQueueStopReserve);
  const auto initially_qualified_participants = qualified_execution_engine_count(registrations_);
  prequalified_max_live_stop_participants_ =
      config_.prequalified_max_live_stop_participants == 0U
          ? std::max<std::size_t>(1U, initially_qualified_participants)
          : config_.prequalified_max_live_stop_participants;
  if (initially_qualified_participants > prequalified_max_live_stop_participants_ ||
      prequalified_max_live_stop_participants_ >
          kMaximumPrequalifiedStopStageAttemptLanes / kMaxStopEscalationStages ||
      prequalified_max_live_stop_participants_ > kMaximumStopControlWorkSlots) {
    throw std::invalid_argument(
        "Stop-qualified runtime participants exceed the prequalified attempt-isolation bound");
  }
  // Stop state (§2.2, §2.4), allocated once: N_max Stop lane indexes
  // (generations from 1), R event records each sized for N_max covered
  // participants, and 2R index buckets. Nothing here is resized later.
  const auto lanes = prequalified_max_live_stop_participants_;
  const auto lane_words = (lanes + 63U) / 64U;
  stop_lanes_.free.resize(lanes);
  stop_lanes_.owners.resize(lanes);
  stop_lane_generations_ = std::make_unique<std::atomic<std::uint32_t>[]>(lanes);
  for (std::size_t index = 0U; index < lanes; ++index) {
    // Popped from the back: index 0 is handed out first.
    stop_lanes_.free[index] = static_cast<std::uint16_t>(lanes - 1U - index);
    stop_lanes_.owners[index].generation = 1U;
    stop_lane_generations_[index].store(1U, std::memory_order_relaxed);
  }
  stop_lanes_.free_count = static_cast<std::uint16_t>(lanes);
  stop_events_.records.resize(config_.request_replay_capacity);
  for (auto& record : stop_events_.records) {
    record.covered.resize(lanes);
    record.covered_lanes.resize(lane_words);
    record.stale.resize(lanes);
  }
  stop_events_.index.assign(2U * config_.request_replay_capacity, kStopRingEmptyBucket);
  stop_touched_operations_.reserve(lanes);
  stop_fence_scratch_ = std::make_unique<ResourceSlotStopFenceScratch>(lanes);
  rebuild_stop_component_table_locked();  // construction is single-threaded
  journal_writer_ = std::make_unique<JournalWriter>(*this);
  stop_control_executor_ = std::make_unique<BoundedStopControlExecutor>(
      lanes, clock_, static_cast<IStopControlWorkReceiver&>(*this));
  if (!stop_control_executor_->prequalified_ready() ||
      stop_control_executor_->capacity() != lanes) {
    throw std::runtime_error("StopControl executor failed its Stop-lane capacity join");
  }
  // Pre-start only the current cell's attempt workers; reconfiguration
  // resizes the pool (up to the ceiling above) at its commit.
  const auto pool_participants = std::max<std::size_t>(1U, initially_qualified_participants);
  stop_stage_attempt_executor_ = std::make_unique<BoundedStopStageAttemptExecutor>(
      pool_participants * kMaxStopEscalationStages, pool_participants,
      static_cast<IStopStageAttemptCompletionReceiver&>(*this));
  if (!stop_stage_attempt_executor_->prequalified_ready() ||
      stop_stage_attempt_executor_->prequalified_max_live_participants() != pool_participants) {
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

  auto begun = journal_.BeginStart(config_.application_start_id, clock_.now(),
                                  config_.request_replay_capacity);
  if (!begun.ok()) {
    throw std::runtime_error("addressed Kernel durable owner admission failed: " +
                             begun.error().code + ": " + begun.error().message);
  }
  cell_admission_owner_ = std::make_unique<KernelCellAdmissionOwner>(
      std::min(config_.request_replay_capacity, config_.cell_admission_claim_capacity), clock_,
      [this](std::span<JournalRecord> records) { post_journal_records(records); },
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
      config_.manager_liveness_required_set,
      [this](const ProgramNodeAdmissionEvidenceDTO& evidence) {
        return validate_program_node_claim(evidence);
      },
      [this](std::shared_ptr<IIoSignalService>* affected_service) {
        std::lock_guard state_lock(state_mutex_);
        return retire_program_blackboard_source_locked(affected_service);
      });
  static_cast<void>(refresh_execution_claims());  // construction is single-threaded; no reader yet

  try {
    for (const auto& registration : registrations_) {
      if (std::any_of(engine_subscriptions_.begin(), engine_subscriptions_.end(),
                      [&](const EngineSubscription& item) {
                        return item.engine == registration.engine;
                      })) {
        continue;
      }
      auto* engine = registration.engine;
      auto component_id = registration.binding.value().controllable_component_id.value();
      if (next_engine_subscription_id_ == std::numeric_limits<std::uint64_t>::max())
        throw std::overflow_error("engine subscription identity exhausted");
      const auto sequence_id = next_engine_subscription_id_++;
      auto ingress = engine_event_ingress_;
      const auto handle = engine->subscribe_events(
          [ingress = std::move(ingress), engine, sequence_id](const KernelEvent& event) {
            auto entered = ingress->Enter();
            if (auto* owner = entered.owner()) owner->on_engine_event(engine, sequence_id, event);
          });
      engine_subscriptions_.push_back(EngineSubscription{engine, handle, sequence_id, std::move(component_id)});
    }
    for (auto& registration : registrations_) {
      const auto subscription = std::find_if(
          engine_subscriptions_.begin(), engine_subscriptions_.end(),
          [&](const EngineSubscription& value) { return value.engine == registration.engine; });
      registration.engine_subscription_id = subscription->sequence_id;
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
  std::vector<ErrorDTO> rule_retire_failures;
  {
    std::lock_guard state_lock(state_mutex_);
    if (program_cycle_claim_run_)
      retire_program_io_watches_locked(program_cycle_claim_run_->program_run_id);
    rule_retire_failures = retire_all_io_rules_locked();
    static_cast<void>(retire_program_blackboard_source_locked());
  }
  for (const auto& failure : rule_retire_failures) {
    const auto reason = failure.details.find("reason");
    (void)std::fprintf(stderr, "manyforge_fault code=%.*s error=%s reason=%s\n",
                       static_cast<int>(error_codes::kIoRuleTeardownRetireFailed.size()),
                       error_codes::kIoRuleTeardownRetireFailed.data(), failure.code.c_str(),
                       reason == failure.details.end() ? "-" : reason->second.c_str());
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
  if (journal_writer_ != nullptr) {
    // Every record posted by the drained lanes is written here, while the
    // journal, clock, scheduler and every participant engine are still alive.
    journal_writer_->Close();
    journal_writer_->ReportQueuedAtDrain();
    WaitForDrain("kernel.journal_writer", std::chrono::seconds(5),
                 [&](std::chrono::nanoseconds bound) {
                   return journal_writer_->DrainFor(bound);
                 });
    journal_writer_->ReportShutdown();
  }
  // Only now may the drained lanes themselves be destroyed. Nothing can reach
  // them afterwards, so a lane cannot post to the journal writer.
  operation_control_executor_.reset();
  stop_control_executor_.reset();
  stop_stage_attempt_executor_.reset();
  journal_writer_.reset();
}

}  // namespace manyforge::core
