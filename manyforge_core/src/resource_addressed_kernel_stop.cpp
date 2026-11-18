#include <manyforge_core/execution_event_change_signal.hpp>
#include "resource_addressed_execution_kernel_internal.hpp"

#include <cstdio>
#include <cstring>

// The Stop core (stop state model, design 2026-11-13 §3): one function,
// `request_stop_locked`, for every Stop; participant stop progress on the
// Stop lane (P1-P7); the failed-Stop handling (F1, F2, F4); the event ring
// reads (K16, K19) and its journal hand-off (§7.1).

namespace manyforge::core {
namespace {

// ---------------------------------------------------------------------------
// Allocation-free helpers used under state_mutex_.
// ---------------------------------------------------------------------------

constexpr std::uint64_t kFnvOffset = 1469598103934665603ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

void hash_bytes(std::uint64_t& hash, const void* data, const std::size_t size) noexcept {
  const auto* bytes = static_cast<const unsigned char*>(data);
  for (std::size_t index = 0U; index < size; ++index) {
    hash ^= bytes[index];
    hash *= kFnvPrime;
  }
}

void hash_u64(std::uint64_t& hash, const std::uint64_t value) noexcept {
  hash_bytes(hash, &value, sizeof(value));
}

void hash_text(std::uint64_t& hash, const std::string_view text) noexcept {
  hash_u64(hash, text.size());
  hash_bytes(hash, text.data(), text.size());
}

/// G1: hash of the normalised request content (kind, selector, run, groups,
/// expected revisions, attribution), walking the fields with no
/// serialisation. Reason and source label are not part of the content.
[[nodiscard]] std::uint64_t stop_request_content_hash(const StopRequest& request) noexcept {
  auto hash = kFnvOffset;
  hash_u64(hash, static_cast<std::uint64_t>(request.kind));
  hash_u64(hash, static_cast<std::uint64_t>(request.preset_widen));
  hash_u64(hash, static_cast<std::uint64_t>(request.selector));
  for (const auto& target : request.operations) {
    hash_u64(hash, target.operation_id.value());
    hash_u64(hash, target.expected_operation_revision.value());
  }
  for (const auto& target : request.components) {
    hash_text(hash, target.controllable_component_id.value());
    hash_text(hash, target.expected_graph_revision.value());
    hash_u64(hash, target.expected_component_lifecycle_generation.value());
  }
  for (const auto& target : request.resources) {
    hash_text(hash, target.resource_id.value());
    hash_text(hash, target.expected_graph_revision.value());
    hash_u64(hash, target.expected_operation_resource_generation);
  }
  hash_u64(hash, request.program_run_id ? request.program_run_id->value() : 0U);
  for (const auto& group : request.group_ids) hash_text(hash, group);
  for (const auto& participant : request.participants) {
    hash_u64(hash, participant.operation_id.value());
    hash_u64(hash, participant.participant_ordinal);
  }
  for (const auto& selection : request.interrupt_selections) {
    hash_u64(hash, selection.operation_id.value());
    hash_u64(hash, selection.participant_ordinal);
    hash_u64(hash, selection.stop_entry);
  }
  if (request.attribution != nullptr) {
    hash_u64(hash, static_cast<std::uint64_t>(request.attribution->transport));
    hash_u64(hash, static_cast<std::uint64_t>(request.attribution->assurance));
    if (request.attribution->principal_id) hash_text(hash, *request.attribution->principal_id);
  }
  hash_u64(hash, request.mode ? static_cast<std::uint64_t>(*request.mode) + 1U : 0U);
  return hash;
}

/// The bucket of a request key in the `2R` open-addressing index.
[[nodiscard]] std::size_t stop_key_bucket(const RequestKey& key, const std::size_t buckets) noexcept {
  return static_cast<std::size_t>((key.high() * 0x9E3779B97F4A7C15ULL) ^ key.low()) % buckets;
}

/// Bounded copy cut on a UTF-8 code-point boundary (never inside a
/// sequence), so an NFC text stays valid NFC for the journal's encoder.
template <std::size_t Capacity>
void copy_text(BoundedStopText<Capacity>& out, const std::string_view text) noexcept {
  auto size = std::min(text.size(), Capacity);
  if (size < text.size()) {
    while (size > 0U && (static_cast<unsigned char>(text[size]) & 0xC0U) == 0x80U) --size;
  }
  if (size != 0U) std::memcpy(out.bytes.data(), text.data(), size);
  out.length = static_cast<std::uint16_t>(size);
}

[[nodiscard]] bool lane_bit(const std::vector<std::uint64_t>& bits, const std::size_t index) noexcept {
  return index / 64U < bits.size() && (bits[index / 64U] & (1ULL << (index % 64U))) != 0U;
}

void set_lane_bit(std::vector<std::uint64_t>& bits, const std::size_t index) noexcept {
  if (index / 64U < bits.size()) bits[index / 64U] |= 1ULL << (index % 64U);
}

void clear_lane_bit(std::vector<std::uint64_t>& bits, const std::size_t index) noexcept {
  if (index / 64U < bits.size()) bits[index / 64U] &= ~(1ULL << (index % 64U));
}

/// `error_code` of a ring entry (contract C7): 1 + the code's position in the
/// generated sorted code array; 0 = none; `kStopErrorCodeUncatalogued`.
[[nodiscard]] std::uint16_t stop_error_code_index(const std::string_view code) noexcept {
  const auto& codes = error_codes::kSortedCodes;
  const auto found = std::lower_bound(codes.begin(), codes.end(), code);
  if (found == codes.end() || *found != code) return kStopErrorCodeUncatalogued;
  return static_cast<std::uint16_t>(1U + static_cast<std::size_t>(found - codes.begin()));
}

[[nodiscard]] std::uint16_t stop_error_code_index(const std::optional<ErrorDTO>& error) noexcept {
  return error.has_value() ? stop_error_code_index(std::string_view(error->code)) : 0U;
}

[[nodiscard]] std::optional<std::string_view> stop_error_code_text(const std::uint16_t index) noexcept {
  const auto& codes = error_codes::kSortedCodes;
  if (index == 0U || index == kStopErrorCodeUncatalogued || index > codes.size())
    return std::nullopt;
  return codes[index - 1U];
}

/// PD-1: the level an invalid request is widened to. An invalid Halt or an
/// unreadable kind becomes Halt; every other invalid Stop becomes Stop all.
[[nodiscard]] StopKind widened_level(const StopKind requested, const StopWidenReason reason) noexcept {
  if (reason == StopWidenReason::kUnreadableKind || requested == StopKind::kHalt ||
      requested == StopKind::kPlannedStop)
    return StopKind::kHalt;
  return StopKind::kStopAll;
}

/// PD-2: the levels that interrupt every component action.
[[nodiscard]] bool interrupts_every_component_action(const StopKind applied) noexcept {
  switch (applied) {
    case StopKind::kStopAll:
    case StopKind::kHalt:
    case StopKind::kCellStop:
    case StopKind::kInternalFault:
    case StopKind::kPlannedStop:
    case StopKind::kStopFailedHalt:
      return true;
    default:
      return false;
  }
}

/// Attribution is raised lexicographically on (class, rule priority).
[[nodiscard]] bool attribution_less(const StopAttribution& left,
                                    const StopAttribution& right) noexcept {
  if (left.attribution_class != right.attribution_class)
    return left.attribution_class < right.attribution_class;
  return left.rule_priority < right.rule_priority;
}

/// The participant ordinal published in event records, replies, `read_stop`
/// and journal rows: the `OperationParticipantId` ordinal (index + 1), as
/// everywhere else. Internal refs (`StopParticipantRef`,
/// `StopInterruptSelection`, the lane owner) keep the index.
[[nodiscard]] constexpr std::uint32_t published_ordinal(const std::size_t index) noexcept {
  return static_cast<std::uint32_t>(index + 1U);
}

[[nodiscard]] StopEventResult completed_result(const StopEventRecord& record) noexcept {
  if (record.covered_count == 0U) return StopEventResult::kNoEffect;
  if (record.any_failed) return StopEventResult::kSomeFailed;
  return record.any_escalated ? StopEventResult::kStoppedEscalated : StopEventResult::kStopped;
}

/// The Manager's carried-object-lost Stop (internal transport, its own
/// label and reason): its participants keep Program-own attribution and end
/// with the typed cause (unchanged).
[[nodiscard]] bool carried_object_lost_request(const StopRequest& request) noexcept {
  return request.kind == StopKind::kOperation && request.attribution != nullptr &&
         request.attribution->transport == RequestTransportDTO::kInternal &&
         request.source_label == "manager.carried_object_lost" &&
         request.reason == "carried_object_lost";
}

[[nodiscard]] RequestReplayStatusDTO replay_status_of(const StopEventRecord& record,
                                                      const std::uint64_t next_before) noexcept {
  return record.event_id.value() < next_before ? RequestReplayStatusDTO::kReplayedExact
                                               : RequestReplayStatusDTO::kAcceptedNew;
}

/// A Stop row posted as a reference into the ring: no allocation.
[[nodiscard]] JournalRecord stop_event_row(const StopEventId event_id, const std::uint32_t slot,
                                           const bool completed) noexcept {
  return JournalRecord{JournalStopEventRow{event_id, slot, completed, {}},
                       completed ? "stop_event_completed" : "stop_event_accepted", std::nullopt,
                       true, false};
}

}  // namespace

// ---------------------------------------------------------------------------
// Entries (§3.2 S2-S5, S16, malformed ingress). Each builds a StopRequest;
// validation that may allocate runs here, before the Kernel lock.
// ---------------------------------------------------------------------------

StopAcceptedDTO ResourceAddressedExecutionKernel::stop(const StopIntentDTO& intent) {
  StopRequest request;
  request.request_key = intent.request_context.request_key;
  request.kind = StopKind::kOperation;
  // An attribution that cannot be kept is recorded unverified, without a
  // principal (as today); the request is still widened as malformed.
  const RequestAttributionDTO unverified{intent.attribution.transport,
                                         RequestAssuranceDTO::kUnverified, std::nullopt};
  request.attribution = stop_intent_field_error(intent) == "stop_request_attribution_invalid"
                            ? &unverified
                            : &intent.attribution;
  request.source_label = intent.request_context.source_label;
  if (intent.reason) request.reason = *intent.reason;
  // S4: operator attribution, except the Manager's carried-object loss.
  request.attribution_rank.attribution_class = carried_object_lost_request(request)
                                                   ? StopAttributionClass::kProgramOwn
                                                   : StopAttributionClass::kOperator;
  if (NormalizedStopIntentFieldError(intent).has_value()) {
    request.preset_widen = StopWidenReason::kMalformed;
    request.selector = StopSelectorKind::kCell;
    return submit_stop(request);
  }
  std::visit(
      [&](const auto& targets) {
        using Targets = std::decay_t<decltype(targets)>;
        if constexpr (std::is_same_v<Targets, std::vector<StopOperationTargetDTO>>) {
          request.selector = StopSelectorKind::kOperations;
          request.operations = targets;
        } else if constexpr (std::is_same_v<Targets, std::vector<StopComponentTargetDTO>>) {
          request.selector = StopSelectorKind::kComponents;
          request.components = targets;
        } else {
          request.selector = StopSelectorKind::kResources;
          request.resources = targets;
        }
      },
      intent.selector);
  return submit_stop(request);
}

namespace {

/// `unverified` (the caller's storage) replaces an attribution that cannot
/// be kept: recorded unverified, without a principal (as today).
[[nodiscard]] StopRequest cell_request(const FullStopIntentDTO& intent, const StopKind kind,
                                       RequestAttributionDTO& unverified) {
  StopRequest request;
  request.request_key = intent.request_context.request_key;
  request.kind = kind;
  request.selector = StopSelectorKind::kCell;
  unverified = RequestAttributionDTO{intent.attribution.transport, RequestAssuranceDTO::kUnverified,
                                     std::nullopt};
  request.attribution = full_stop_intent_field_error(intent) == "full_stop_request_attribution_invalid"
                            ? &unverified
                            : &intent.attribution;
  request.source_label = intent.request_context.source_label;
  request.attribution_rank.attribution_class = StopAttributionClass::kOperator;
  if (full_stop_intent_field_error(intent).has_value())
    request.preset_widen = StopWidenReason::kMalformed;
  return request;
}

}  // namespace

StopAcceptedDTO ResourceAddressedExecutionKernel::stop_all(const FullStopIntentDTO& intent) {
  RequestAttributionDTO unverified;
  return submit_stop(cell_request(intent, StopKind::kStopAll, unverified));
}

StopAcceptedDTO ResourceAddressedExecutionKernel::halt(const FullStopIntentDTO& intent) {
  RequestAttributionDTO unverified;
  // S17: the planned application stop is a Halt recorded as `planned_stop`;
  // the host and the Manager send an internal `halt` with their own label
  // (contract §14 (2)). Everything else of it is a Halt.
  const bool planned_stop =
      intent.attribution.transport == RequestTransportDTO::kInternal &&
      (intent.request_context.source_label == "kernel.planned_application_stop" ||
       intent.request_context.source_label == "manager.planned_application_stop");
  return submit_stop(cell_request(intent, planned_stop ? StopKind::kPlannedStop : StopKind::kHalt, unverified));
}

StopAcceptedDTO ResourceAddressedExecutionKernel::cell_stop(const FullStopIntentDTO& intent) {
  RequestAttributionDTO unverified;
  return submit_stop(cell_request(intent, StopKind::kCellStop, unverified));
}

StopAcceptedDTO ResourceAddressedExecutionKernel::end_program_run(
    const ProgramRunEndIntentDTO& intent) {
  StopRequest request;
  request.request_key = intent.request_context.request_key;
  request.kind = StopKind::kProgramEnd;
  request.selector = StopSelectorKind::kProgramRun;
  request.program_run_id = intent.program_run_id;
  request.attribution = &intent.attribution;
  request.source_label = intent.request_context.source_label;
  request.attribution_rank.attribution_class = StopAttributionClass::kOperator;
  if (program_run_end_intent_field_error(intent).has_value()) {
    request.preset_widen = StopWidenReason::kMalformed;
    request.selector = StopSelectorKind::kCell;
  }
  return submit_stop(request);
}

StopAcceptedDTO ResourceAddressedExecutionKernel::recognized_stop(
    const RecognizedStopIngressDTO& ingress) {
  // PD-1 by recognised route: the group, operation and Stop-all routes widen
  // to Stop all, the Halt route to Halt, an unknown route (unreadable kind)
  // to Halt. A Stop naming another or no application start is recorded as
  // `stale_start`, every other one as `malformed`. An attribution that
  // cannot be kept is recorded unverified; the caller's label is recorded
  // when the transport passed a valid one.
  RequestAttributionDTO attribution = ingress.attribution;
  if (recognized_stop_ingress_field_error(ingress) == "recognized_stop_ingress_attribution_invalid")
    attribution = RequestAttributionDTO{attribution.transport, RequestAssuranceDTO::kUnverified,
                                        std::nullopt};
  StopRequest request;
  request.request_key = ingress.request_key;
  request.selector = StopSelectorKind::kCell;
  request.attribution = &attribution;
  request.source_label =
      ingress.source_label.has_value() && stop_source_label_valid(*ingress.source_label)
          ? std::string_view(*ingress.source_label)
          : std::string_view("recognized_stop_ingress");
  request.reason = ingress.reason;
  request.attribution_rank.attribution_class = StopAttributionClass::kOperator;
  if (ingress.recognized_route == "/api/execution/stops") {
    request.kind = StopKind::kOperation;
    request.preset_widen = StopWidenReason::kMalformed;
  } else if (ingress.recognized_route == "/api/execution/stop-all") {
    request.kind = StopKind::kStopAll;
    request.preset_widen = StopWidenReason::kMalformed;
  } else if (ingress.recognized_route == "/api/execution/halt") {
    request.kind = StopKind::kHalt;
    request.preset_widen = StopWidenReason::kMalformed;
  } else {
    request.kind = StopKind::kHalt;
    request.preset_widen = StopWidenReason::kUnreadableKind;
  }
  if (request.preset_widen == StopWidenReason::kMalformed &&
      ingress.reason == "recognized_stop_start_not_current")
    request.preset_widen = StopWidenReason::kStaleStart;
  return submit_stop(request);
}

void ResourceAddressedExecutionKernel::stop_program_run_after_runner_loss(
    const ProgramRunNumber& program_run_id) noexcept {
  try {
    // Once per loss: the caller calls it when a loss follows a report without one.
    StopRequest request;
    request.request_key = mint_kernel_request_key();
    request.kind = StopKind::kRunnerLoss;
    request.selector = StopSelectorKind::kProgramRun;
    request.program_run_id = program_run_id;
    request.source_label = "kernel.program_runner_loss";
    request.reason = "program_runner_loss";
    request.attribution_rank.attribution_class = StopAttributionClass::kOperator;
    static_cast<void>(submit_stop(request));
  } catch (...) {
    // Only the reply's allocation can throw; the Stop is already applied.
  }
}

// ---------------------------------------------------------------------------
// Hand-off and reply (§3.1 after-unlock steps, §7.2).
// ---------------------------------------------------------------------------

StopHandoff& ResourceAddressedExecutionKernel::thread_stop_handoff() {
  // N1 (coordinator): owned per accepting thread, sized on the thread's
  // first Stop for N_max and the component table, before the lock is taken.
  thread_local StopHandoff handoff;
  const auto lanes = prequalified_max_live_stop_participants_;
  const auto lane_words = (lanes + 63U) / 64U;
  if (handoff.lanes.size() < lane_words) handoff.lanes.resize(lane_words, 0U);
  if (handoff.fence_completions.size() < lanes) handoff.fence_completions.resize(lanes);
  const auto component_words =
      (stop_component_count_.load(std::memory_order_acquire) + 63U) / 64U;
  if (handoff.interrupt_components.size() < component_words)
    handoff.interrupt_components.resize(component_words, 0U);
  return handoff;
}

StopEventRecord& ResourceAddressedExecutionKernel::thread_stop_record_copy() {
  thread_local StopEventRecord copy;
  const auto lanes = prequalified_max_live_stop_participants_;
  if (copy.covered.size() != lanes) copy.covered.resize(lanes);
  if (copy.stale.size() != lanes) copy.stale.resize(lanes);
  const auto lane_words = (lanes + 63U) / 64U;
  if (copy.covered_lanes.size() != lane_words) copy.covered_lanes.resize(lane_words);
  return copy;
}

void ResourceAddressedExecutionKernel::copy_stop_event_locked(const std::uint32_t record_index,
                                                              StopEventRecord& out) const noexcept {
  // Equal sizes: copy-assignment reuses the destination's storage.
  out = stop_events_.records[record_index];
}

StopAcceptedDTO ResourceAddressedExecutionKernel::stop_accepted_from_record(
    const StopEventRecord& record, const RequestReplayStatusDTO replay_status) {
  StopAcceptedDTO accepted;
  accepted.request_key = record.request_key;
  accepted.replay_status = replay_status;
  accepted.event_id = record.event_id;
  accepted.requested = record.requested;
  accepted.applied = record.applied;
  accepted.widen_reason = record.widen_reason;
  if (record.run_number.nonzero()) accepted.program_run_id = record.run_number;
  accepted.stale_diagnostics.reserve(record.stale_count);
  for (std::size_t index = 0U; index < record.stale_count; ++index) {
    const auto& stale = record.stale[index];
    accepted.stale_diagnostics.push_back({stale.operation_id, stale.expected, stale.actual});
  }
  accepted.stale_total = record.stale_total;
  accepted.covered.reserve(record.covered_count);
  for (std::size_t index = 0U; index < record.covered_count; ++index) {
    const auto& entry = record.covered[index];
    accepted.covered.push_back({entry.operation_id, entry.participant_ordinal, entry.started});
  }
  accepted.hold_revision = record.hold_revision_after;
  accepted.accepted_at_ns = record.accepted_at_ns;
  return accepted;
}

StopAcceptedDTO ResourceAddressedExecutionKernel::submit_stop(const StopRequest& request) {
  auto& handoff = thread_stop_handoff();
  auto& copy = thread_stop_record_copy();
  RequestReplayStatusDTO replay_status{RequestReplayStatusDTO::kAcceptedNew};
  {
    std::lock_guard state_lock(state_mutex_);
    const auto next_before = next_stop_event_id_;
    const auto index = request_stop_locked(request, handoff);
    copy_stop_event_locked(index, copy);
    replay_status = replay_status_of(copy, next_before);
  }
  complete_stop_handoff(handoff);
  return stop_accepted_from_record(copy, replay_status);
}

void ResourceAddressedExecutionKernel::complete_stop_handoff(StopHandoff& handoff) noexcept {
  // (1) Engine fence call (2) for every call (1) made (closed gates or none),
  // before any Stop work starts (§3.1; nothing can be dispatched in between).
  for (std::size_t index = 0U; index < handoff.fence_completion_count; ++index) {
    const auto& completion = handoff.fence_completions[index];
    completion.engine->complete_addressed_participant_stop_fence(completion.execution_id,
                                                                 completion.closed_gates);
  }
  // (2) Publish each started lane with its generation (mirrored under the
  // lock when the lane was popped).
  for (std::size_t word = 0U; word < handoff.lanes.size(); ++word) {
    auto bits = handoff.lanes[word];
    while (bits != 0U) {
      const auto bit = static_cast<std::size_t>(std::countr_zero(bits));
      bits &= bits - 1U;
      const auto lane = word * 64U + bit;
      if (lane >= prequalified_max_live_stop_participants_) continue;
      stop_control_executor_->Publish(StopControlWorkToken{
          static_cast<StopControlSlotIndex>(lane),
          stop_lane_generations_[lane].load(std::memory_order_acquire)});
    }
  }
  // (3) The event-stream change signal once.
  if (handoff.fence_changed) execution_event_change_signal().Notify();
  // (4) The component-action listener (PD-2), no lock, no copy.
  if (handoff.accepted) {
    if (auto* listener = stop_listener_.load(std::memory_order_acquire)) {
      listener->OnStopAccepted(StopComponentInterrupts{
          handoff.interrupt_all_components,
          std::span<const std::uint64_t>(handoff.interrupt_components),
          handoff.component_table.get()});
    }
  }
  // (5) Wake the journal writer for the Stop-lane records posted under the lock.
  if (handoff.journal_pending) wake_journal_writer();
  // Clear for reuse; nothing is freed but the table reference.
  std::fill(handoff.lanes.begin(), handoff.lanes.end(), 0U);
  handoff.fence_completion_count = 0U;
  handoff.fence_changed = false;
  handoff.accepted = false;
  handoff.interrupt_all_components = false;
  std::fill(handoff.interrupt_components.begin(), handoff.interrupt_components.end(), 0U);
  handoff.component_table.reset();
  handoff.journal_pending = false;
}

// ---------------------------------------------------------------------------
// The one Stop core (§3.1 G1-G8, E1-E13). Under state_mutex_: memory work,
// the slot-table leaf (E1), engine call (1) (E2, and E11 inside K2's G3);
// no allocation, print, wait or callback.
// ---------------------------------------------------------------------------

std::uint32_t ResourceAddressedExecutionKernel::request_stop_locked(const StopRequest& request,
                                                                    StopHandoff& handoff) noexcept {
  const auto now = clock_.now();
  auto& ring = stop_events_;
  const auto record_capacity = static_cast<std::uint32_t>(ring.records.size());
  const auto buckets = ring.index.size();

  // ---- G1 Duplicate.
  const auto content_hash = stop_request_content_hash(request);
  StopWidenReason widen = request.preset_widen;
  std::optional<std::size_t> key_bucket;
  std::optional<std::size_t> free_bucket;
  {
    auto bucket = stop_key_bucket(request.request_key, buckets);
    for (std::size_t probe = 0U; probe < buckets; ++probe, bucket = (bucket + 1U) % buckets) {
      const auto slot = ring.index[bucket];
      if (slot == kStopRingEmptyBucket) {
        if (!free_bucket) free_bucket = bucket;
        break;
      }
      if (slot == kStopRingTombstoneBucket) {
        if (!free_bucket) free_bucket = bucket;
        continue;
      }
      if (ring.records[slot].event_id.nonzero() &&
          ring.records[slot].request_key == request.request_key) {
        key_bucket = bucket;
        break;
      }
    }
  }
  if (key_bucket) {
    const auto slot = ring.index[*key_bucket];
    if (ring.records[slot].content_hash == content_hash) return slot;  // replayed exact
    if (widen == StopWidenReason::kNone) widen = StopWidenReason::kKeyConflict;
  }

  // ---- G5 (slot): the record at the ring head, overwriting the oldest.
  const auto record_index = ring.head;
  ring.head = (ring.head + 1U) % record_capacity;
  auto& record = ring.records[record_index];
  if (record.event_id.nonzero()) {
    // Its index bucket becomes a tombstone.
    auto bucket = stop_key_bucket(record.request_key, buckets);
    for (std::size_t probe = 0U; probe < buckets; ++probe, bucket = (bucket + 1U) % buckets) {
      const auto slot = ring.index[bucket];
      if (slot == kStopRingEmptyBucket) break;
      if (slot == record_index) {
        ring.index[bucket] = kStopRingTombstoneBucket;
        if (key_bucket && *key_bucket == bucket) key_bucket.reset();
        break;
      }
    }
  }
  const StopEventId event_id(next_stop_event_id_++);
  record.event_id = event_id;
  record.request_key = request.request_key;
  record.content_hash = content_hash;
  record.requested = request.kind;
  record.applied = request.kind;
  record.caused_by = request.caused_by;
  record.widen_reason = StopWidenReason::kNone;
  record.transport = request.attribution ? request.attribution->transport
                                         : RequestTransportDTO::kInternal;
  record.assurance = request.attribution ? request.attribution->assurance
                                         : RequestAssuranceDTO::kUnverified;
  copy_text(record.principal, request.attribution && request.attribution->principal_id
                                  ? std::string_view(*request.attribution->principal_id)
                                  : std::string_view{});
  copy_text(record.reason, request.reason);
  copy_text(record.source_label, request.source_label);
  record.run_number = request.program_run_id.value_or(ProgramRunNumber(0U));
  record.group_count = 0U;
  record.capture.reset();  // W11: may free a retired run's capture table
  record.hold_change = 0U;
  record.hold_revision_after = hold_revision_;
  record.covered_count = 0U;
  std::fill(record.covered_lanes.begin(), record.covered_lanes.end(), 0U);
  record.outstanding = 0U;
  record.fence_degraded = false;
  record.fence_degraded_reason = 0U;
  record.stale_count = 0U;
  record.stale_total = 0U;
  record.any_failed = false;
  record.any_escalated = false;
  record.accepted_at_ns = now;
  record.completed_at_ns = 0;
  record.result = StopEventResult::kPending;
  // Index the key: a conflicting request takes over its key's bucket.
  if (key_bucket) {
    ring.index[*key_bucket] = record_index;
  } else if (free_bucket) {
    ring.index[*free_bucket] = record_index;
  } else {
    auto bucket = stop_key_bucket(request.request_key, buckets);
    for (std::size_t probe = 0U; probe < buckets; ++probe, bucket = (bucket + 1U) % buckets) {
      if (ring.index[bucket] == kStopRingEmptyBucket ||
          ring.index[bucket] == kStopRingTombstoneBucket) {
        ring.index[bucket] = record_index;
        break;
      }
    }
  }

  // ---- G2 Validate and widen. A Stop is never refused.
  ProgramCycleClaimRun* claim =
      program_cycle_claim_run_.has_value() ? &*program_cycle_claim_run_ : nullptr;
  StopSelectorKind selector = request.selector;
  bool no_effect = false;
  const auto add_stale = [&](const OperationNumber& operation, const OperationRevision expected,
                             const OperationRevision actual) {
    if (record.stale_count < record.stale.size())
      record.stale[record.stale_count++] = StopStaleEntry{operation, expected, actual};
    ++record.stale_total;
  };
  // A target naming a pending (or queued) takeover newcomer names its
  // incumbent, the only physical owner (as today).
  const auto incumbent_of_candidate = [&](const OperationNumber& id) -> ActiveOperation* {
    for (auto& [incumbent_id, incumbent] : active_operations_) {
      static_cast<void>(incumbent_id);
      if ((!incumbent->pending_motion_takeover.empty() &&
           incumbent->pending_motion_takeover.key() == id) ||
          (!incumbent->next_motion_takeover.empty() &&
           incumbent->next_motion_takeover.key() == id))
        return incumbent.get();
    }
    return nullptr;
  };
  const auto pending_candidate_revision = [&](const ActiveOperation& incumbent,
                                              const OperationNumber& id) {
    if (!incumbent.pending_motion_takeover.empty() && incumbent.pending_motion_takeover.key() == id)
      return incumbent.pending_motion_takeover.mapped()->state.operation_revision;
    return incumbent.next_motion_takeover.mapped()->state.operation_revision;
  };
  if (widen == StopWidenReason::kNone) {
    switch (selector) {
      case StopSelectorKind::kOperations:
        for (const auto& target : request.operations) {
          std::optional<OperationRevision> current;
          bool needs_containment = false;
          if (const auto* operation = find_active_operation_locked(target.operation_id)) {
            current = operation->state.operation_revision;
            needs_containment =
                stop_requires_containment(operation->state.state,
                                          operation->containment_effect_uncertain()) ||
                std::any_of(operation->participants.begin(), operation->participants.end(),
                            [](const auto& row) { return row.stop.phase == StopPhase::kFailed; });
          } else if (const auto* incumbent = incumbent_of_candidate(target.operation_id)) {
            current = pending_candidate_revision(*incumbent, target.operation_id);
            needs_containment = true;
          }
          const bool historical =
              !current.has_value() && historical_operations_.contains(target.operation_id);
          switch (classify_stop_operation_target(target, current, needs_containment, historical)) {
            case StopTargetResolution::kUnknown:
              widen = StopWidenReason::kUnknownTarget;
              break;
            case StopTargetResolution::kStaleMetadata:
              add_stale(target.operation_id, target.expected_operation_revision, *current);
              break;
            case StopTargetResolution::kExact:
            case StopTargetResolution::kNoEffect:
              break;
          }
        }
        break;
      case StopSelectorKind::kComponents:
        for (const auto& target : request.components) {
          if (find_stop_component(stop_component_scope_, target.controllable_component_id) ==
              nullptr)
            widen = StopWidenReason::kUnknownTarget;
        }
        break;
      case StopSelectorKind::kResources:
        for (const auto& target : request.resources) {
          if (find_stop_resource_owner(stop_component_scope_, target.resource_id) == nullptr)
            widen = StopWidenReason::kUnknownTarget;
        }
        break;
      case StopSelectorKind::kProgramGroups:
      case StopSelectorKind::kProgramRun: {
        // PD-13 as decided: the current run acts; a run not current and not
        // above the highest run started has no effect; a run above it is
        // invalid.
        const auto run = request.program_run_id.value_or(ProgramRunNumber(0U));
        if (claim != nullptr && claim->program_run_id == run) {
          if (selector == StopSelectorKind::kProgramGroups && claim->capture &&
              !select_program_groups_locked(*claim, request.group_ids))
            widen = StopWidenReason::kUnknownTarget;
        } else if (run.nonzero() && run.value() <= highest_program_run_started_.value()) {
          no_effect = true;
        } else {
          widen = StopWidenReason::kUnknownTarget;
        }
        break;
      }
      case StopSelectorKind::kCell:
      case StopSelectorKind::kParticipants:
      case StopSelectorKind::kInterruptSelections:
        break;
    }
  }
  if (widen != StopWidenReason::kNone) {
    record.widen_reason = widen;
    record.applied = widened_level(request.kind, widen);
    selector = StopSelectorKind::kCell;
    no_effect = false;
  }
  const auto applied = record.applied;
  // Group indexes for History: the first 16 named selections of the capture.
  if (selector == StopSelectorKind::kProgramGroups && claim != nullptr && claim->capture) {
    record.capture = claim->capture;
    const auto& selections = claim->capture->table->named_selections;
    for (const auto& group : request.group_ids) {
      const auto found = std::find_if(selections.begin(), selections.end(), [&](const auto& row) {
        return row.group_id == group;
      });
      // Index == named_selections.size() names the built-in `all` group
      // (the journal codec's encoding, contract §13).
      const auto index = found != selections.end()
                             ? static_cast<std::size_t>(found - selections.begin())
                             : (group == "all" ? selections.size() : selections.size() + 1U);
      if (index > selections.size()) continue;
      if (record.group_count < kStopEventGroupSlots)
        record.groups[record.group_count] = static_cast<std::uint16_t>(index);
      ++record.group_count;
    }
  }

  // ---- G3 Raise hold (K2: rows S1-S18 as a join; E11 call (1) inside).
  bool raise_revision = false;
  if (!no_effect) raise_revision = apply_stop_hold_locked(request, applied, record, handoff);

  // ---- G4 Cover and start.
  const bool all_components = interrupts_every_component_action(applied);
  if (all_components) handoff.interrupt_all_components = true;
  const auto mark_component = [&](const ControllableComponentId& component) {
    if (handoff.interrupt_all_components) return;
    const auto index = stop_component_index_locked(component);
    if (!index) return;
    if (*index / 64U >= handoff.interrupt_components.size()) {
      // The cell grew after this thread sized its hand-off: interrupt every
      // action rather than fewer than asked.
      handoff.interrupt_all_components = true;
      return;
    }
    handoff.interrupt_components[*index / 64U] |= 1ULL << (*index % 64U);
  };
  // PD-2 (1): the selector's own components, and (contract §14 (4)) each
  // one's owner: an owner's Reset position runs through its arm member.
  if (!no_effect && selector == StopSelectorKind::kComponents) {
    for (const auto& target : request.components) {
      mark_component(target.controllable_component_id);
      for (const auto& state : component_states_) {
        if (state.component_id != target.controllable_component_id.value() || !state.owner_id)
          continue;
        if (handoff.interrupt_all_components || !stop_component_table_) break;
        const auto& table = *stop_component_table_;
        const auto found = std::lower_bound(
            table.begin(), table.end(), std::string_view(*state.owner_id),
            [](const ControllableComponentId& row, std::string_view id) { return row.value() < id; });
        if (found == table.end() || found->value() != *state.owner_id) break;
        const auto index = static_cast<std::size_t>(found - table.begin());
        if (index / 64U >= handoff.interrupt_components.size())
          handoff.interrupt_all_components = true;
        else
          handoff.interrupt_components[index / 64U] |= 1ULL << (index % 64U);
        break;
      }
    }
  }
  if (!no_effect && selector == StopSelectorKind::kResources)
    for (const auto& target : request.resources)
      if (const auto* owner = find_stop_resource_owner(stop_component_scope_, target.resource_id))
        mark_component(owner->controllable_component_id);
  // PD-2 (3): a group Stop interrupts the components bound to its nodes.
  if (!no_effect && selector == StopSelectorKind::kProgramGroups && claim != nullptr &&
      claim->capture && claim->capture->table && claim->capture->binding) {
    const auto& nodes = claim->capture->table->nodes;
    for (std::size_t node = 0U; node < nodes.size(); ++node) {
      if (!lane_bit(claim->hold.selected_nodes, node)) continue;
      for (const auto& effect : nodes[node].target_effects) {
        if (!effect.effectful) continue;
        for (const auto& target : claim->capture->binding->target_bindings) {
          if (target.target != effect.target) continue;
          for (const auto& role : target.resource_role_bindings)
            for (const auto& resource : role.resource_ids)
              if (const auto* owner = find_stop_resource_owner(stop_component_scope_, resource))
                mark_component(owner->controllable_component_id);
        }
      }
    }
  }

  // Which operations and participants the selector names.
  const auto operation_selected = [&](const OperationNumber& id, const ActiveOperation& operation) {
    switch (selector) {
      case StopSelectorKind::kCell:
        return true;
      case StopSelectorKind::kOperations:
        return std::any_of(request.operations.begin(), request.operations.end(),
                           [&](const auto& target) {
                             if (target.operation_id == id) return true;
                             // A target naming this incumbent's newcomer.
                             return (!operation.pending_motion_takeover.empty() &&
                                     operation.pending_motion_takeover.key() == target.operation_id) ||
                                    (!operation.next_motion_takeover.empty() &&
                                     operation.next_motion_takeover.key() == target.operation_id);
                           });
      case StopSelectorKind::kComponents:
        return std::any_of(operation.participants.begin(), operation.participants.end(),
                           [&](const auto& participant) {
                             return !participant.ownership_transferred &&
                                    std::any_of(request.components.begin(),
                                                request.components.end(), [&](const auto& target) {
                                                  return target.controllable_component_id ==
                                                         participant.binding.value()
                                                             .controllable_component_id;
                                                });
                           });
      case StopSelectorKind::kResources:
        return std::any_of(operation.participants.begin(), operation.participants.end(),
                           [&](const auto& participant) {
                             return !participant.ownership_transferred &&
                                    std::any_of(request.resources.begin(), request.resources.end(),
                                                [&](const auto& target) {
                                                  return std::find(participant.resources.begin(),
                                                                   participant.resources.end(),
                                                                   target.resource_id) !=
                                                         participant.resources.end();
                                                });
                           });
      case StopSelectorKind::kProgramGroups: {
        // S1: the run's Program operations whose node is selected (whole
        // operations; the coordinated closure was applied by the selection).
        if (claim == nullptr || !claim->capture || !claim->capture->table ||
            operation.state.program_run_id != claim->program_run_id)
          return false;
        const auto& nodes = claim->capture->table->nodes;
        return std::any_of(
            operation.state.participants.begin(), operation.state.participants.end(),
            [&](const auto& row) {
              if (!row.execution_scope) return false;
              const auto found = std::lower_bound(
                  nodes.begin(), nodes.end(), row.execution_scope->node_instance_id,
                  [](const auto& node, const auto& instance) { return node.instance_id < instance; });
              return found != nodes.end() &&
                     found->instance_id == row.execution_scope->node_instance_id &&
                     lane_bit(claim->hold.selected_nodes,
                              static_cast<std::size_t>(found - nodes.begin()));
            });
      }
      case StopSelectorKind::kProgramRun: {
        // S10, S16: the run's Program and handover operations.
        const auto run = request.program_run_id.value_or(ProgramRunNumber(0U));
        return operation.state.program_run_id == run ||
               (operation.manual_handover && operation.manual_handover->capture.program_run_id == run);
      }
      case StopSelectorKind::kParticipants:
        return std::any_of(request.participants.begin(), request.participants.end(),
                           [&](const auto& row) { return row.operation_id == id; });
      case StopSelectorKind::kInterruptSelections:
        return std::any_of(request.interrupt_selections.begin(), request.interrupt_selections.end(),
                           [&](const auto& row) { return row.operation_id == id; });
    }
    return false;
  };
  const auto participant_selected = [&](const OperationNumber& id, const std::size_t ordinal) {
    switch (selector) {
      case StopSelectorKind::kParticipants:
        return std::any_of(request.participants.begin(), request.participants.end(),
                           [&](const auto& row) {
                             return row.operation_id == id && row.participant_ordinal == ordinal;
                           });
      case StopSelectorKind::kInterruptSelections:
        return std::any_of(request.interrupt_selections.begin(), request.interrupt_selections.end(),
                           [&](const auto& row) {
                             return row.operation_id == id && row.participant_ordinal == ordinal;
                           });
      default:
        return true;  // whole operations (review M6)
    }
  };
  const auto interrupt_selection_for = [&](const OperationNumber& id,
                                           const std::size_t ordinal) -> const StopInterruptSelection* {
    for (const auto& row : request.interrupt_selections)
      if (row.operation_id == id && row.participant_ordinal == ordinal) return &row;
    return nullptr;
  };
  // A participant released before dispatch whose slots no longer name it had
  // no effect and keeps it (today's `released_pre_dispatch`). The slot read is
  // the table's allocation-free owner query (slot-table leaf, §9.3).
  const auto released_pre_dispatch = [&](const ActiveOperationParticipant& participant) {
    return participant.stop_cancelled_pre_dispatch && !participant.prepared_dispatch_released &&
           participant.slot_state == ResourceSlotState::kTerminal &&
           !participant.resources.empty() &&
           !resource_slots_->owner_holds_any(participant.slot_owner, participant.resources);
  };
  // The row's typed cause follows the label and reason on any transport;
  // only the internal Manager Stop is Program-own (carried_object_lost_request).
  const bool carried_object_lost = request.kind == StopKind::kOperation &&
                                   request.source_label == "manager.carried_object_lost" &&
                                   request.reason == "carried_object_lost";
  const bool runtime_collision = applied == StopKind::kRuntimeCollision;
  const bool internal_control = applied == StopKind::kOperationReset ||
                                applied == StopKind::kHoldTimeout;
  const bool failure_halt = applied == StopKind::kStopFailedHalt;
  StopAttribution rank = request.attribution_rank;
  if (widen != StopWidenReason::kNone) rank = StopAttribution{StopAttributionClass::kOperator, 0, 0U};

  // K3: the base reused an earlier Stop's settled proof instead of capturing
  // an operation again (2bf4744a `resource_addressed_kernel_stop.cpp:793-972`,
  // `has_settled_proof`). The same facts, from the participants' own stop
  // states: nothing of the operation in flight (control, Stop, dispatch,
  // recovery, coordinated cancellation, preparation borrow, deferred
  // settlement), not Interrupted, its revision unchanged since that Stop's
  // acceptance, no participant failed, and every motion participant (pneumatic
  // non-motion transactions and transferred rows aside) dispatched,
  // contained and settled (phase stopped, not cancelled before dispatch, no
  // failed preparation, no lost interrupt source), at least one of them. A
  // widened (malformed or invalid) request never reuses a proof (as today).
  const bool proof_reuse = widen == StopWidenReason::kNone;
  const auto operation_stop_proven = [](const ActiveOperation& operation) {
    if (!operation.state.stop_event_id.has_value() || operation.pending_control.has_value() ||
        operation.stop_effect_in_flight || operation.dispatch_effect_in_flight() ||
        operation.recovery_effect_in_flight || operation.coordinated_cancellation_in_flight ||
        operation.state.state == OperationStateKind::kInterrupted)
      return false;
    bool matched = false;
    for (std::size_t index = 0U; index < operation.participants.size(); ++index) {
      const auto& participant = operation.participants[index];
      if (participant.stop.phase == StopPhase::kFailed) return false;
      if (participant.non_motion_transaction() || participant.ownership_transferred) continue;
      const auto& work = participant.stop_work;
      if (participant.dispatch_effect_in_flight || participant.reconciliation_in_flight ||
          participant.prepare_in_flight || participant.prepared_advance_in_flight ||
          participant.preparation_borrow_in_flight() ||
          participant.deferred_stop_settlement.has_value())
        return false;
      if (participant.stop.phase != StopPhase::kStopped || work.cancelled_pre_dispatch ||
          work.preparation_failed || !work.participant_settlement_complete ||
          work.participant_settlement_error.has_value() ||
          operation.state.operation_revision != work.accepted_operation_revision)
        return false;
      if (index < operation.state.participants.size() &&
          std::any_of(operation.state.participants[index].io_rule_outcomes.begin(),
                      operation.state.participants[index].io_rule_outcomes.end(),
                      [](const auto& outcome) { return !outcome.evidence.source_losses.empty(); }))
        return false;
      matched = true;
    }
    return matched;
  };
  stop_touched_operations_.clear();
  // E1 rows go into the Kernel-owned scratch (K2 type, reserved at
  // construction), canonical by (operation, participant): the walk is in
  // operation order and participants in ordinal order.
  stop_fence_scratch_->clear();
  bool fence_rows = false;

  if (!no_effect) {
    for (auto& [operation_id, operation_ptr] : active_operations_) {
      auto& operation = *operation_ptr;
      if (!operation_selected(operation_id, operation)) continue;
      const bool uncertain = operation.containment_effect_uncertain();
      const bool operation_in_scope =
          stop_requires_containment(operation.state.state, uncertain);
      // K3 (base settled-proof reuse): an operation whose participants' own
      // stop states prove them stopped is not captured again.
      const bool proven = proof_reuse && operation_stop_proven(operation);
      bool covered_here = false;
      bool started_here = false;
      for (std::size_t ordinal = 0U; ordinal < operation.participants.size(); ++ordinal) {
        auto& participant = operation.participants[ordinal];
        if (!participant_selected(operation_id, ordinal)) continue;
        auto& stop = participant.stop;
        // Stop-qualified: Stop readiness, a Stop port, a prepared plan; a
        // pneumatic non-motion transaction keeps today's revision-only row.
        const bool qualified = participant.stop_readiness.has_value() &&
                               participant.stop_execution_port != nullptr &&
                               participant.ordinary_stop_plan != nullptr &&
                               !participant.non_motion_transaction() &&
                               !released_pre_dispatch(participant);
        if (!qualified) continue;
        if (!operation_in_scope && stop.phase != StopPhase::kFailed) continue;
        mark_component(participant.binding.value().controllable_component_id);
        if (stop.phase == StopPhase::kStopping) {
          // Covered, not restarted: only attribution rises.
          if (attribution_less(stop.attribution, rank)) stop.attribution = rank;
          if (record.covered_count < record.covered.size()) {
            auto& entry = record.covered[record.covered_count++];
            entry = StopCoveredEntry{operation_id, published_ordinal(ordinal),
                                     stop.lane_index, stop.lane_generation, false, false,
                                     StopPhase::kStopping, kNoStopStage,
                                     StopEscalationStageKind::kNominal, false, false, 0U};
            set_lane_bit(record.covered_lanes, stop.lane_index);
          }
          covered_here = true;
          continue;
        }
        // Contained: no effect, unless the operation is effect-uncertain
        // without a proof of its stopped participants (P6, as today).
        if (stop.phase == StopPhase::kStopped && (!uncertain || proven)) continue;
        // ---- Start a new attempt (P1/P6).
        const auto* selection = interrupt_selection_for(operation_id, ordinal);
        StopMode mode = StopMode::kOrdinary;
        const PreparedParticipantStopPlanDTO* plan = participant.ordinary_stop_plan.get();
        bool lane_hard_stop = false;
        if (selection != nullptr && selection->plan != nullptr) {
          mode = StopMode::kInterrupt;
          plan = selection->plan;
        } else if (runtime_collision) {
          mode = StopMode::kFault;
          if (participant.payload_kind == OperationParticipantPayloadKind::kMotion &&
              participant.fault_stop_plan != nullptr)
            plan = participant.fault_stop_plan.get();
        } else if (applied == StopKind::kHoldTimeout ||
                   (applied == StopKind::kOperationReset &&
                    !participant.default_operator_stop_profile.has_value())) {
          mode = StopMode::kHard;
          lane_hard_stop = true;
        } else if (request.mode.has_value() && widen == StopWidenReason::kNone) {
          mode = *request.mode;
        }
        const auto previous_phase = stop.phase;
        stop.attempt = static_cast<std::uint16_t>(stop.attempt + 1U);
        stop.mode = mode;
        stop.plan = plan;
        stop.interrupt_entry = selection ? selection->stop_entry : kNoStopStage;
        stop.interrupt_success_through = selection ? selection->success_through : kNoStopStage;
        stop.containing_stage = kNoStopStage;
        stop.escalated = false;
        stop.stop_path_collision = false;
        stop.error.reset();
        stop.started_by = event_id;
        stop.attribution = rank;
        auto& work = participant.stop_work;
        work.reset_for_attempt();
        work.payload_kind = participant.payload_kind;
        work.accepted_at_ns = now;
        work.prepared_execution_id = participant.preparation_id;
        work.prepared_motion_engine = participant.prepared_motion_engine;
        work.execution_engine = participant.engine;
        work.runtime_collision_containment = runtime_collision;
        work.carried_object_lost = carried_object_lost;
        work.internal_control_stop = internal_control;
        work.started_by_failure_halt = failure_halt;
        work.takeover_stop = request.own_takeover_stop;
        work.force_spring_return = mode == StopMode::kSpringReturn;
        if (selection != nullptr) {
          work.current_stage_index = selection->stop_entry;
          work.selected_interrupt_suffix = true;
        } else if (runtime_collision &&
                   participant.payload_kind == OperationParticipantPayloadKind::kMotion &&
                   participant.fault_stop_plan == nullptr) {
          // Admission required this exact prepared cancel row; no profile is
          // invented.
          work.current_stage_index = controller_goal_cancel_stage_index(*plan);
        }
        work.lane_hard_stop_requested = lane_hard_stop && !plan->stage_commands.empty();
        if (work.lane_hard_stop_requested) work.current_stage_index = plan->stage_commands.size() - 1U;
        // Stop lane index (§2.2). The bound excludes underflow; if it ever
        // happens that one participant fails typed, everything else proceeds.
        if (stop_lanes_.free_count == 0U) {
          stop.phase = StopPhase::kFailed;
          stop.lane_index = kNoStopLane;
          stop.error = error("stop_lane_index_exhausted",
                             "No Stop lane index was free for this participant",
                             "stop_lane_index_exhausted");
          count_participant_phase_locked(participant, previous_phase, StopPhase::kFailed);
          // The participant's attempt ended (typed failure): its rule rows
          // get the outcome as at every other end (P4).
          finalize_io_rule_outcomes_locked(operation, ordinal);
          record.any_failed = true;
          if (record.covered_count < record.covered.size()) {
            record.covered[record.covered_count++] = StopCoveredEntry{
                operation_id, published_ordinal(ordinal), kNoStopLane, 0U, true, true,
                StopPhase::kFailed, kNoStopStage, StopEscalationStageKind::kNominal, false, false,
                stop_error_code_index(stop.error)};
          }
          refresh_cell_latch_locked(true);
        } else {
          const auto lane = stop_lanes_.free[--stop_lanes_.free_count];
          auto& owner = stop_lanes_.owners[lane];
          owner.operation_id = operation_id;
          owner.participant_ordinal = static_cast<std::uint32_t>(ordinal);
          stop.lane_index = lane;
          stop.lane_generation = owner.generation;
          stop.phase = StopPhase::kStopping;
          ++stopping_count_;
          count_participant_phase_locked(participant, previous_phase, StopPhase::kStopping);
          set_lane_bit(handoff.lanes, lane);
          set_lane_bit(record.covered_lanes, lane);
          if (record.covered_count < record.covered.size()) {
            record.covered[record.covered_count++] = StopCoveredEntry{
                operation_id, published_ordinal(ordinal), lane, owner.generation, true,
                false, StopPhase::kStopping, kNoStopStage,
                                     StopEscalationStageKind::kNominal, false, false, 0U};
          }
        }
        started_here = true;
        covered_here = true;
      }
      // E13 for every covered operation: its takeover candidates lose their
      // right to commit, unless this is that takeover's own Stop (S12).
      if (covered_here && !request.own_takeover_stop &&
          (!operation.pending_motion_takeover.empty() || !operation.next_motion_takeover.empty()))
        operation.takeover_cancelled = true;
      // Base (2bf4744a `resource_addressed_kernel_stop.cpp:1573-1577`,
      // :1719-1735): an operation in scope whose participants are all
      // non-motion transactions gets no engine fence and no containment
      // ownership, but its revision is raised (revision-only slot rows), its
      // control superseded, its Stop id set and its state captured.
      const bool revision_only_operation =
          !started_here && !covered_here && operation_in_scope &&
          std::any_of(operation.participants.begin(), operation.participants.end(),
                      [&](const auto& row) {
                        return row.non_motion_transaction() && !released_pre_dispatch(row);
                      }) &&
          std::none_of(operation.participants.begin(), operation.participants.end(),
                       [&](const auto& row) {
                         return !row.non_motion_transaction() && !released_pre_dispatch(row) &&
                                row.stop.phase != StopPhase::kRunning;
                       });
      if (!started_here && !revision_only_operation) continue;
      // ---- E1 rows for this operation (fence applied once for the event).
      auto revision_before = operation.state.operation_revision;
      if (operation.pending_control.has_value() &&
          operation.pending_control->accepted.operation_revision_after_acceptance > revision_before)
        revision_before = operation.pending_control->accepted.operation_revision_after_acceptance;
      const OperationRevision next_revision(revision_before.value() + 1U);
      for (std::size_t ordinal = 0U; ordinal < operation.participants.size(); ++ordinal) {
        auto& participant = operation.participants[ordinal];
        if (released_pre_dispatch(participant) || participant.resources.empty()) continue;
        // Started by this event (also a typed lane-underflow failure, whose
        // slots are fenced all the same).
        const bool fenced = participant.stop.started_by == event_id;
        stop_fence_scratch_->add_owner(participant.slot_owner, participant.resources,
                                       next_revision, !fenced);
        fence_rows = true;
      }
      if (stop_touched_operations_.size() < stop_touched_operations_.capacity())
        stop_touched_operations_.push_back(&operation);
      operation.state.operation_revision = next_revision;  // E4 (applied below too)
    }
  }

  // ---- E1 Slot-table Stop fence, one batch for the event (slot-table leaf).
  bool fence_failed = false;
  if (fence_rows) {
    if (const auto refused = resource_slots_->stop_fence_batch(event_id, *stop_fence_scratch_)) {
      // Accepted-degraded: logged by the journal writer (X2), never here.
      fence_failed = true;
      record.fence_degraded = true;
      record.fence_degraded_reason = stop_error_code_index(*refused);
    }
  }

  // ---- E2-E9 per operation with a started participant.
  for (auto* operation_ptr : stop_touched_operations_) {
    auto& operation = *operation_ptr;
    const auto next_revision = operation.state.operation_revision;
    const auto operation_id = operation.state.operation_id;
    for (auto& participant : operation.participants) {
      const bool started = participant.stop.started_by == event_id;
      if (!started) continue;
      // E2 call (1): the linearising part only (atomics, pointer copy,
      // gripper state assign); call (2) runs after the unlock.
      if (participant.engine != nullptr && participant.preparation_id != 0U) {
        const auto fenced = participant.engine->fence_addressed_participant_stop(
            participant.preparation_id);
        participant.stop_cancelled_pre_dispatch =
            participant.stop_cancelled_pre_dispatch ||
            fenced.disposition == AddressedParticipantStopFenceDisposition::kEffectPreventedBeforeDispatch;
        // The motion's committed send has not returned: this Stop's
        // settlement waits for the participant's report of its outcome.
        if (fenced.send_outcome_pending &&
            participant.payload_kind == OperationParticipantPayloadKind::kMotion)
          participant.motion_send_outcome_awaited = participant.preparation_id;
        // Call (2) runs for every call (1) that named an execution, also
        // with no gate closed: it revokes the lookahead permits as today.
        if (handoff.fence_completion_count < handoff.fence_completions.size()) {
          handoff.fence_completions[handoff.fence_completion_count++] = StopEngineFenceCompletion{
              participant.engine, participant.preparation_id, fenced.closed_gates};
        }
      }
    }
    // E3
    if (operation.pending_control.has_value()) operation.pending_control->superseded_by_stop = true;
    // E4
    if (operation.runtime_scale_status.has_value()) {
      operation.runtime_scale_status->operation_revision = next_revision;
      operation.runtime_scale_status->updated_at_ns = now;
    }
    for (std::size_t ordinal = 0U; ordinal < operation.participants.size(); ++ordinal) {
      auto& participant = operation.participants[ordinal];
      const bool started = participant.stop.started_by == event_id;
      if (!started) continue;
      // E5
      participant.stop_containment_owned = true;
      participant.stop_cancelled_pre_dispatch =
          participant.stop_cancelled_pre_dispatch ||
          (participant.payload_kind == OperationParticipantPayloadKind::kMotion
               ? !participant.prepared_dispatch_released
               : (!participant.prepared_dispatch_released &&
                  participant.slot_state == ResourceSlotState::kReserved));
      participant.stop_work.cancelled_pre_dispatch = participant.stop_cancelled_pre_dispatch;
      participant.stop_work.accepted_operation_revision = next_revision;
      // E6 participant row
      if (ordinal < operation.state.participants.size())
        operation.state.participants[ordinal].state =
            participant.stop_cancelled_pre_dispatch ? OperationStateKind::kCancelledPreDispatch
                                                    : OperationStateKind::kStopping;
    }
    // E6 operation
    operation.state.state = OperationStateKind::kStopping;
    operation.state.stop_event_id = event_id;
    operation.state.updated_at_ns = now;
    // E7: keep the Kernel's owner equal to the table's (a refused fence
    // changed nothing there).
    if (!fence_failed) {
      for (auto& participant : operation.participants) {
        participant.slot_owner.operation_revision = next_revision;
        if (participant.stop.started_by == event_id && participant.stop_owns_reconciliation())
          participant.slot_state = ResourceSlotState::kStopping;
      }
    }
    if (std::none_of(operation.participants.begin(), operation.participants.end(),
                     [](const auto& row) { return row.stop_owns_reconciliation(); }))
      operation.state.state = aggregate_participant_state(operation.state.participants);
    // E8 only with a started participant: a revision-only operation has no
    // Stop work to wait for (the base cleared it on the Stop lane's first
    // pass), E9 always.
    if (std::any_of(operation.participants.begin(), operation.participants.end(),
                    [&](const auto& row) { return row.stop.started_by == event_id; }))
      operation.stop_effect_in_flight = true;
    stamp_operation_capture_locked(operation, operation.state);
    static_cast<void>(operation_id);
  }

  // ---- G5 Record.
  std::uint16_t outstanding = 0U;
  for (std::size_t index = 0U; index < record.covered_count; ++index)
    if (!record.covered[index].done) ++outstanding;
  record.outstanding = outstanding;
  if (outstanding == 0U) {
    record.result = completed_result(record);
    record.completed_at_ns = now;
  }

  // ---- G6 Revision and markers.
  if (raise_revision) ++hold_revision_;
  record.hold_revision_after = hold_revision_;
  last_stop_event_id_ = event_id;

  // ---- G7 Latch, G8 hand-off.
  refresh_cell_latch_locked(true);
  handoff.fence_changed = true;
  handoff.accepted = true;
  post_stop_journal_record_locked(stop_event_row(event_id, record_index, false));
  if (record.result != StopEventResult::kPending)
    post_stop_journal_record_locked(stop_event_row(event_id, record_index, true));
  handoff.journal_pending = true;
  if (!handoff.component_table) handoff.component_table = stop_component_table_;
  return record_index;
}

// ---------------------------------------------------------------------------
// P4/P5: one participant's attempt ended (with F1 and F2).
// ---------------------------------------------------------------------------

ParticipantStopOutcomeDTO ResourceAddressedExecutionKernel::participant_stop_outcome(
    const ActiveOperationParticipant& participant) noexcept {
  const auto& stop = participant.stop;
  return ParticipantStopOutcomeDTO{stop.started_by, stop.phase,    stop.mode,
                                   stop.containing_stage, stop.escalated, stop.attempt};
}

std::optional<StopControlWorkToken> ResourceAddressedExecutionKernel::wake_participant_stop_locked(
    const ActiveOperationParticipant& participant) noexcept {
  if (participant.stop.phase != StopPhase::kStopping ||
      participant.stop.lane_index == kNoStopLane)
    return std::nullopt;
  const StopControlWorkToken token{participant.stop.lane_index, participant.stop.lane_generation};
  stop_control_executor_->ArmDeadline(token, clock_.now());
  return token;
}

void ResourceAddressedExecutionKernel::capture_stop_operation_state_locked(
    ActiveOperation& operation, const char* reason) {
  stamp_operation_capture_locked(operation, operation.state);
  auto copy = operation.state;
  fill_participant_stop_observations(operation.participants, copy);
  // Stop lane: the caller sets `handoff.journal_pending`, which wakes the writer.
  post_operation_state_locked(operation,
                              JournalOperationState{std::move(copy), std::nullopt, reason, {}},
                              reason, std::nullopt, true);
}

void ResourceAddressedExecutionKernel::complete_participant_stop_locked(
    ActiveOperation& operation, const std::size_t participant_ordinal,
    const std::int64_t completed_at_ns, StopHandoff& handoff) noexcept {
  auto& participant = operation.participants[participant_ordinal];
  auto& stop = participant.stop;
  auto& work = participant.stop_work;
  const auto lane = stop.lane_index;
  const auto lane_generation = stop.lane_generation;
  const auto operation_id = operation.state.operation_id;

  // (1) Outcome. Contained (stopped) when a stage contained it and its
  // settlement succeeded; uncontained (failed) otherwise.
  const bool contained = work.contained && !work.participant_settlement_error.has_value();
  stop.phase = contained ? StopPhase::kStopped : StopPhase::kFailed;
  // `error` is the degraded-Stop or failure cause. The typed cause a
  // contained Stop leaves on its row (the carried object lost, the runtime
  // collision it contained) is neither.
  const auto* state_row = participant_ordinal < operation.state.participants.size()
                              ? &operation.state.participants[participant_ordinal]
                              : nullptr;
  const bool typed_cause_only =
      state_row != nullptr && contained && !work.stop_path_collision.has_value() &&
      ((work.carried_object_lost && state_row->state == OperationStateKind::kStopped) ||
       (work.runtime_collision_containment && participant.runtime_collision_error.has_value() &&
        state_row->terminal_error == participant.runtime_collision_error));
  if (!typed_cause_only && state_row != nullptr && state_row->terminal_error.has_value()) {
    stop.error = state_row->terminal_error;
  } else if (!contained) {
    stop.error = work.participant_settlement_error ? work.participant_settlement_error
                                                   : work.terminal_error;
  }
  std::optional<std::size_t> containing;
  for (const auto& retained : work.retained_results) {
    if (!retained.authoritative ||
        retained.result.outcome != ParticipantStopStageOutcomeDTO::kSucceeded ||
        retained.result.error.has_value())
      continue;
    const auto index = retained.result.request.stage_index;
    if (!containing || index > *containing) containing = index;
  }
  stop.containing_stage = containing && *containing < kNoStopStage
                              ? static_cast<std::uint8_t>(*containing)
                              : kNoStopStage;
  stop.escalated = contained && work.escalation_cause.has_value() && !work.lane_hard_stop_requested;
  stop.stop_path_collision = work.stop_path_collision.has_value();
  // K11: once the incumbent's attempt started by the takeover's own Stop
  // ended, the takeover decision names that Stop event (it replaces the
  // certificate reference).
  if (!operation.pending_motion_takeover.empty()) {
    auto& candidate = *operation.pending_motion_takeover.mapped();
    if (candidate.motion_takeover_spring_fallback && candidate.state.motion_takeover &&
        candidate.motion_takeover_spring_fallback->stop_event_id == stop.started_by) {
      candidate.state.motion_takeover->stop_event_id = stop.started_by;
      stamp_operation_capture_locked(candidate, candidate.state);
    }
  }

  // (2) Lane release, counters, markers.
  if (lane != kNoStopLane) {
    stop_control_executor_->CancelDeadline(StopControlWorkToken{lane, lane_generation});
    auto& owner = stop_lanes_.owners[lane];
    owner.operation_id = OperationNumber(0U);
    owner.participant_ordinal = 0U;
    owner.generation = lane_generation + 1U;
    stop_lane_generations_[lane].store(owner.generation, std::memory_order_release);
    stop_lanes_.free[stop_lanes_.free_count++] = lane;
  }
  stop.lane_index = kNoStopLane;
  if (stopping_count_ > 0U) --stopping_count_;
  count_participant_phase_locked(participant, StopPhase::kStopping, stop.phase);
  ++stop_progress_sequence_;
  handoff.fence_changed = true;
  finalize_io_rule_outcomes_locked(operation, participant_ordinal);
  if (std::none_of(operation.participants.begin(), operation.participants.end(),
                   [](const auto& row) { return row.stop.phase == StopPhase::kStopping; }))
    operation.stop_effect_in_flight = false;

  // (4) Ring completion: entries matched by (lane index, generation).
  if (lane != kNoStopLane) {
    const auto error_code = stop_error_code_index(stop.error);
    const auto containing_stage_kind =
        stop.plan != nullptr && stop.containing_stage != kNoStopStage &&
                stop.containing_stage < stop.plan->stage_commands.size()
            ? stop.plan->stage_commands[stop.containing_stage].resolved_stage.stage_kind
            : StopEscalationStageKind::kNominal;
    for (auto& record : stop_events_.records) {
      if (!record.event_id.nonzero() || record.result != StopEventResult::kPending ||
          !lane_bit(record.covered_lanes, lane))
        continue;
      for (std::size_t index = 0U; index < record.covered_count; ++index) {
        auto& entry = record.covered[index];
        if (entry.done || entry.lane_index != lane || entry.lane_generation != lane_generation)
          continue;
        entry.done = true;
        entry.phase = stop.phase;
        entry.containing_stage = stop.containing_stage;
        entry.containing_stage_kind = containing_stage_kind;
        entry.escalated = stop.escalated;
        entry.stop_path_collision = stop.stop_path_collision;
        entry.error_code = error_code;
        record.any_failed = record.any_failed || stop.phase == StopPhase::kFailed;
        record.any_escalated = record.any_escalated || stop.escalated;
        if (record.outstanding > 0U) --record.outstanding;
      }
      clear_lane_bit(record.covered_lanes, lane);
      if (record.outstanding == 0U) {
        record.completed_at_ns = completed_at_ns;
        record.result = completed_result(record);
        post_stop_journal_record_locked(stop_event_row(
            record.event_id, static_cast<std::uint32_t>(&record - stop_events_.records.data()), true));
        handoff.journal_pending = true;
      }
    }
  }

  if (stop.phase != StopPhase::kFailed) return;
  // F2: the component is stop-unconfirmed until its controller reports
  // idle in a state arriving after now; the existing latch holds meanwhile.
  if (const auto index =
          stop_component_index_locked(participant.binding.value().controllable_component_id))
    stop_unconfirmed_.marked[*index] = 1U;
  refresh_cell_latch_locked(true);
  // F1: a failed Stop raises a Halt, once per failure; a failure of an
  // attempt started by such a Halt raises none (one-level bound). Admission
  // is not closed (internal_fault_ is not set).
  if (work.started_by_failure_halt) return;
  StopRequest halt;
  halt.request_key = mint_kernel_request_key();
  halt.kind = StopKind::kStopFailedHalt;
  halt.selector = StopSelectorKind::kCell;
  halt.source_label = kStopFailedSourceLabel;
  halt.reason = stop.error ? std::string_view(stop.error->code) : std::string_view{};
  halt.attribution_rank.attribution_class = StopAttributionClass::kOperator;
  halt.caused_by = StopCausedBy{stop.started_by, operation_id,
                                published_ordinal(participant_ordinal)};
  static_cast<void>(request_stop_locked(halt, handoff));
}

// ---------------------------------------------------------------------------
// Reads of the event ring (K16, K19).
// ---------------------------------------------------------------------------

Result<StopEventStatusDTO> ResourceAddressedExecutionKernel::read_stop(
    const StopEventId event_id) const {
  std::lock_guard state_lock(state_mutex_);
  // Event n is written at ring index (n - 1) mod R (one record per event).
  const auto& records = stop_events_.records;
  if (!event_id.nonzero() || records.empty() ||
      records[(event_id.value() - 1U) % records.size()].event_id != event_id) {
    return reject<StopEventStatusDTO>("Stop event is not resident", "stop_status_not_resident",
                                      ErrorCode::kMissingReference);
  }
  const auto& record = records[(event_id.value() - 1U) % records.size()];
  StopEventStatusDTO status;
  status.event_id = record.event_id;
  status.request_key = record.request_key;
  status.requested = record.requested;
  status.applied = record.applied;
  status.widen_reason = record.widen_reason;
  status.complete = record.result != StopEventResult::kPending;
  status.result = record.result;
  status.covered.reserve(record.covered_count);
  for (std::size_t index = 0U; index < record.covered_count; ++index) {
    const auto& entry = record.covered[index];
    StopEventCoveredOutcomeDTO row{entry.operation_id, entry.participant_ordinal, entry.started,
                                   entry.done,         entry.phase};
    if (entry.done && entry.containing_stage != kNoStopStage)
      row.containing_stage_index = entry.containing_stage;
    row.escalated = entry.escalated;
    row.stop_path_collision = entry.stop_path_collision;
    if (const auto text = stop_error_code_text(entry.error_code)) row.error_code = std::string(*text);
    status.covered.push_back(std::move(row));
  }
  status.accepted_at_ns = record.accepted_at_ns;
  if (status.complete) status.completed_at_ns = record.completed_at_ns;
  return Result<StopEventStatusDTO>::Ok(std::move(status));
}

CellStopActionProjectionDTO ResourceAddressedExecutionKernel::cell_stop_action_projection() const {
  std::lock_guard state_lock(state_mutex_);
  CellStopActionProjectionDTO projection;
  if (last_stop_event_id_.nonzero()) projection.last_event_id = last_stop_event_id_;
  const StopEventRecord* newest = nullptr;
  for (const auto& record : stop_events_.records) {
    if (!record.event_id.nonzero() || record.result == StopEventResult::kPending) continue;
    if (newest == nullptr || record.completed_at_ns > newest->completed_at_ns ||
        (record.completed_at_ns == newest->completed_at_ns && record.event_id > newest->event_id))
      newest = &record;
  }
  if (newest != nullptr) {
    projection.last_completed = CellStopLastCompletedDTO{
        newest->event_id, newest->applied, newest->result,
        newest->result == StopEventResult::kStoppedEscalated, newest->completed_at_ns};
  }
  return projection;
}

// ---------------------------------------------------------------------------
// Stop lane: stage-attempt completions and participant progress (§3.3).
// ---------------------------------------------------------------------------

void ResourceAddressedExecutionKernel::StageAttemptCompleted(
    BoundedStopStageAttemptExecutor& source, const StopStageAttemptHandle handle) noexcept {
  std::optional<StopControlWorkToken> owner_token;
  try {
    {
      std::lock_guard state_lock(state_mutex_);
      // Only the current pool's attempts can belong to a stopping
      // participant: a pool is swapped out only while nothing stops.
      if (&source == stop_stage_attempt_executor_.get()) {
        for (std::size_t lane = 0U; lane < stop_lanes_.owners.size() && !owner_token; ++lane) {
          const auto& owner = stop_lanes_.owners[lane];
          if (!owner.operation_id.nonzero()) continue;
          const auto* operation = find_active_operation_locked(owner.operation_id);
          if (operation == nullptr || owner.participant_ordinal >= operation->participants.size())
            continue;
          const auto& participant = operation->participants[owner.participant_ordinal];
          if (participant.stop.phase != StopPhase::kStopping || participant.stop.lane_index != lane)
            continue;
          const auto& attempts = participant.stop_work.attempts;
          if (std::any_of(attempts.begin(), attempts.end(), [&](const auto& attempt) {
                return attempt.handle.has_value() && *attempt.handle == handle &&
                       !attempt.result_consumed;
              })) {
            owner_token = StopControlWorkToken{static_cast<StopControlSlotIndex>(lane),
                                               participant.stop.lane_generation};
            stop_control_executor_->ArmDeadline(*owner_token, clock_.now());
          }
        }
      }
    }
    if (owner_token.has_value()) {
      stop_control_executor_->Publish(*owner_token);
      return;
    }
    // A late result of an ended attempt is consumed and released so it never
    // exhausts Stop-stage capacity.
    const auto late = source.TryTake(handle);
    if (late.status != StopStageAttemptTakeStatus::kPending) {
      static_cast<void>(source.Release(handle));
    }
  } catch (...) {
    // Only a defect throws here; no lock is held once the try has unwound.
    ContainInternalFault(error_codes::kStopStageCompletionException);
  }
}

void ResourceAddressedExecutionKernel::ProgressStop(const StopControlWorkToken token) noexcept {
  try {
    // Late cancellation replies of never-dispatched participants whose
    // Stop already ended are tidied up on this lane, before any Stop work.
    settle_deferred_pre_dispatch_cancellations();
    auto& handoff = thread_stop_handoff();
    std::optional<OperationNumber> takeover_source_wakeup;
    const ScopeExit wake_takeover([&] {
      if (!takeover_source_wakeup) return;
      try {
        static_cast<void>(advance_motion_takeover(*takeover_source_wakeup));
      } catch (...) {
      }
    });
    struct PreparedCancellation final {
      OperationParticipantPayloadKind payload_kind{OperationParticipantPayloadKind::kMotion};
      IResolvedPreparedParticipantExecutionEngine* engine{};
      IResolvedParticipantExecutionEngine* execution_engine{};
      std::uint64_t execution_id{};
    };
    struct ParticipantSettlement final {
      IResolvedParticipantExecutionEngine* engine{};
      AddressedParticipantStopSettlementRequestDTO request;
    };
    std::optional<PreparedCancellation> prepared_cancellation;
    std::optional<PreparedCancellation> prepared_classification;
    std::optional<ParticipantSettlement> participant_settlement;
    std::vector<OperationControlEffectRequest> reset_effects;
    std::optional<OperationNumber> operation_settled;
    // Re-find the participant this token's lane belongs to.
    const auto participant_for_token = [&]() -> std::pair<ActiveOperation*, std::size_t> {
      if (!token.locally_valid() || token.slot >= stop_lanes_.owners.size()) return {nullptr, 0U};
      const auto& owner = stop_lanes_.owners[token.slot];
      if (owner.generation != token.generation || !owner.operation_id.nonzero())
        return {nullptr, 0U};
      auto* operation = find_active_operation_locked(owner.operation_id);
      if (operation == nullptr || owner.participant_ordinal >= operation->participants.size())
        return {nullptr, 0U};
      const auto& participant = operation->participants[owner.participant_ordinal];
      if (participant.stop.phase != StopPhase::kStopping ||
          participant.stop.lane_index != token.slot)
        return {nullptr, 0U};
      return {operation, owner.participant_ordinal};
    };
    {
      std::lock_guard state_lock(state_mutex_);
      const auto found = participant_for_token();
      auto* const operation_ptr = found.first;
      const auto ordinal = found.second;
      if (operation_ptr == nullptr) return;
      // A candidate between its hand-over's "decide" and "close" steps
      // (OD-175 D3, gates 2-3): the engine has not adopted it yet. Nothing is
      // consumed here (escalation input stays pending); the close step's
      // wake arms this lane again.
      if (operation_ptr->participants[ordinal].handover_source_execution_id != 0U) return;
      auto& operation = *operation_ptr;
      auto& participant = operation.participants[ordinal];
      auto& item = participant.stop_work;
      if (item.progress_in_flight) return;
      item.progress_in_flight = true;
      const auto operation_id = operation.state.operation_id;
      const auto stop_event = participant.stop.started_by;
      const auto& plan = *participant.stop.plan;
      const auto& controller_goal_id = operation.state.participants[ordinal].command_id;
      if (item.takeover_stop) takeover_source_wakeup = operation_id;
      if (operation.journaled_stop_event_id != operation.state.stop_event_id.value_or(StopEventId(0U))) {
        // The operation's stopping state (E6) for its own journal row.
        operation.journaled_stop_event_id = operation.state.stop_event_id.value_or(StopEventId(0U));
        capture_stop_operation_state_locked(operation, "stop_accepted");
        handoff.journal_pending = true;
      }
      const auto now = clock_.now();
      std::optional<std::int64_t> next_deadline;
      const auto retain_first_error = [&](ErrorDTO failure) {
        if (!item.terminal_error.has_value()) item.terminal_error = std::move(failure);
      };
      const auto advance_to_strongest = [&] {
        // The sealed chain is strength-ordered. Nominal failure/expiry goes
        // straight to its strongest authorized action.
        const auto count = plan.stage_commands.size();
        item.current_stage_index = item.current_stage_index + 1U < count ? count - 1U : count;
      };
      const auto advance_after_failed_stage = [&] {
        if (item.selected_interrupt_suffix) {
          if (item.current_stage_index < plan.stage_commands.size()) ++item.current_stage_index;
        } else {
          advance_to_strongest();
        }
      };

      if (item.collision_escalation_pending) {
        // A collision on the Stop path aborts the running stage at once and
        // selects the strongest authorized stage; the participant then ends
        // InterventionRequired with the collision.
        item.collision_escalation_pending = false;
        auto collision_error = std::move(item.collision_escalation_error);
        item.collision_escalation_error.reset();
        if (!item.terminal && !item.cancelled_pre_dispatch && collision_error.has_value()) {
          if (!item.stop_path_collision.has_value()) item.stop_path_collision = collision_error;
          const auto strongest =
              plan.stage_commands.empty() ? std::size_t{0} : plan.stage_commands.size() - 1U;
          if (item.current_stage_index < strongest) {
            auto& running = item.attempts[item.current_stage_index];
            if (running.started && !running.result_consumed && !running.superseded) {
              running.superseded = true;
              if (running.handle.has_value())
                static_cast<void>(stop_stage_attempt_executor_->RequestCancel(*running.handle));
            }
            retain_first_error(std::move(*collision_error));
            advance_to_strongest();
          }
        }
      }
      if (item.cancelled_pre_dispatch && !item.terminal) {
        bool preparation_in_flight = false;
        if (item.prepared_execution_id == 0U) {
          preparation_in_flight = participant.prepare_in_flight || participant.prepared_advance_in_flight;
          if (!preparation_in_flight) item.prepared_execution_id = participant.preparation_id;
          item.prepared_motion_engine = participant.prepared_motion_engine;
          item.execution_engine = participant.engine;
          if (participant.last_subordinate_error.has_value())
            retain_first_error(*participant.last_subordinate_error);
        }
        const bool needs_cancellation =
            item.prepared_motion_engine != nullptr && item.prepared_execution_id != 0U;
        if (needs_cancellation && !item.prepared_cancel_requested) {
          item.prepared_cancel_requested = true;
          prepared_cancellation = PreparedCancellation{item.payload_kind, item.prepared_motion_engine,
                                                       item.execution_engine,
                                                       item.prepared_execution_id};
        }
        const bool waiting =
            preparation_in_flight || (needs_cancellation && !item.prepared_cancel_complete);
        const bool cancel_terminal_outstanding =
            needs_cancellation && item.prepared_cancel_complete &&
            item.payload_kind == OperationParticipantPayloadKind::kMotion &&
            !item.prepared_cancel_terminal_observed &&
            !item.unclassified_prepared_terminal.has_value();
        if (waiting) {
          // No shared deadline; the never-dispatched participant's own owner
          // settles it (its completion wakes this lane).
        } else if (cancel_terminal_outstanding) {
          // Nothing was dispatched and the dispatch barrier stays closed:
          // the Stop closes now; settlement waits for the cancel terminal.
          item.cancel_terminal_deferred = true;
          item.contained = true;
          item.terminal = true;
        } else if (item.unclassified_prepared_terminal.has_value()) {
          if (item.prepared_motion_engine == nullptr || item.prepared_execution_id == 0U) {
            fail_prepared_cancellation(item, *item.unclassified_prepared_terminal);
          } else if (!item.prepared_classification_requested) {
            item.prepared_classification_requested = true;
            prepared_classification =
                PreparedCancellation{item.payload_kind, item.prepared_motion_engine,
                                     item.execution_engine, item.prepared_execution_id};
          }
        } else {
          item.contained = true;
          item.terminal = true;
        }
      }

      const auto expire_attempt = [&](StopParticipantAttempt& attempt, const TimePoint observed_at) {
        if (!attempt.started || attempt.result_consumed || attempt.superseded ||
            !attempt.stage_deadline_ns.has_value() || observed_at < *attempt.stage_deadline_ns)
          return false;
        attempt.superseded = true;
        if (attempt.handle.has_value())
          static_cast<void>(stop_stage_attempt_executor_->RequestCancel(*attempt.handle));
        const bool controlled =
            attempt.stage && attempt.stage->stage_index < plan.stage_commands.size() &&
            plan.stage_commands[attempt.stage->stage_index]
                    .resolved_stage.command_qualification.stop_action_class ==
                StopActionClass::kControlled;
        if (controlled) {
          retain_first_error(error(std::string(to_string(ErrorCode::kExecutionTimeout)),
                                   "Controlled Stop did not prove containment before its deadline",
                                   "participant_stop_stage_deadline_expired"));
        } else {
          // P7 (F4): the software-hard stage's port never reported.
          retain_first_error(error("stop_stage_watchdog_expired",
                                   "The Stop stage's port did not report before the Kernel's "
                                   "watchdog of the stage expired",
                                   "stop_stage_watchdog_expired"));
        }
        advance_after_failed_stage();
        return true;
      };
      for (auto& attempt : item.attempts) {
        if (!attempt.started || attempt.result_consumed || !attempt.handle.has_value()) continue;
        if (item.takeover_stop && !operation.pending_motion_takeover.empty()) {
          // S12: hand the spring-return point of this attempt to the
          // takeover fallback that waits for this exact event.
          auto& candidate = *operation.pending_motion_takeover.mapped();
          auto& fallback = candidate.motion_takeover_spring_fallback;
          if (fallback && !fallback->return_point && fallback->stop_event_id == stop_event &&
              operation.state.stop_event_id == stop_event) {
            auto point = stop_stage_attempt_executor_->ReadSpringReturnPoint(*attempt.handle);
            if (point && point->source_goal == controller_goal_id &&
                point->binding_epoch == point->trip.binding_epoch) {
              fallback->stage = attempt.stage;
              fallback->return_point = std::move(point);
            }
          }
        }
        auto result = stop_stage_attempt_executor_->TryTake(*attempt.handle);
        const auto observed_at = clock_.now();
        // Deadline precedence applies even when a result is already queued.
        static_cast<void>(expire_attempt(attempt, observed_at));
        if (result.status == StopStageAttemptTakeStatus::kPending) continue;
        attempt.result_consumed = true;
        static_cast<void>(stop_stage_attempt_executor_->Release(*attempt.handle));
        if (result.status == StopStageAttemptTakeStatus::kCompleted && result.result.has_value()) {
          auto valid = ValidateParticipantStopStageResult(
              *result.result, participant.stop_readiness->value().resolved_stop_base_plan,
              participant.binding, participant.retained_default_stop_limits,
              participant.stop_node_category, participant.resources);
          if (valid.ok()) {
            item.retained_results.push_back(StopRetainedStageResult{
                item.next_result_sequence++, observed_at, !attempt.superseded,
                std::move(*result.result)});
            if (!attempt.superseded) {
              const auto& retained = item.retained_results.back().result;
              if (retained.error.has_value()) retain_first_error(*retained.error);
              if (retained.outcome == ParticipantStopStageOutcomeDTO::kSucceeded) {
                item.contained = true;
                item.terminal = true;
                if (retained.request.stage_index > 0U && item.terminal_error.has_value()) {
                  item.escalation_cause = std::move(item.terminal_error);
                  // A hard stop the Stop planner chose is the intended Stop.
                  if (planner_selected_hard_stop(*item.escalation_cause))
                    item.lane_hard_stop_requested = true;
                }
                item.terminal_error.reset();
              } else if (retained.request.stage_index == 0U &&
                         controlled_stop_unsettled_result(retained)) {
                // `main`'s confirmed-but-not-settled case: no second command.
                item.controlled_stop_unsettled = true;
                item.terminal = true;
                item.contained = false;
                if (item.terminal_error.has_value() && item.terminal_error->details.size() <= 6U) {
                  item.terminal_error->details.insert_or_assign(
                      "emergency_fallback", "not_attempted_controlled_stop_already_confirmed");
                  item.terminal_error->details.insert_or_assign("hard_stop_transitioned", "false");
                }
              } else {
                advance_after_failed_stage();
              }
            }
          } else if (!attempt.superseded) {
            retain_first_error(valid.error());
            advance_after_failed_stage();
          }
        } else if (!attempt.superseded) {
          retain_first_error(error(std::string(to_string(ErrorCode::kInternalError)),
                                   "Stop-stage execution lane failed without a result",
                                   "stop_stage_attempt_infrastructure_failure"));
          advance_after_failed_stage();
        }
      }

      // A never-dispatched participant that is not yet terminal waits for its
      // own prepared owner (cancellation or classification above): no stage.
      while (!item.terminal && !item.cancelled_pre_dispatch) {
        if (item.current_stage_index >= plan.stage_commands.size()) {
          // P5: the cascade is exhausted.
          item.terminal = true;
          item.contained = false;
          if (!item.terminal_error.has_value())
            item.terminal_error = error(std::string(to_string(ErrorCode::kExecutionRejected)),
                                        "Stop cascade exhausted without proof of containment",
                                        "participant_stop_cascade_exhausted");
          break;
        }
        auto& attempt = item.attempts[item.current_stage_index];
        if (attempt.started && !attempt.result_consumed) {
          if (expire_attempt(attempt, clock_.now())) continue;
          if (attempt.stage_deadline_ns.has_value())
            next_deadline = !next_deadline ? attempt.stage_deadline_ns
                                           : std::min(*next_deadline, *attempt.stage_deadline_ns);
          break;
        }
        if (attempt.started) {
          advance_after_failed_stage();
          continue;
        }
        const auto dispatch_now = clock_.now();
        const auto& command = plan.stage_commands[item.current_stage_index];
        auto budget = remaining_stop_budget(command, stop_event, dispatch_now, item.accepted_at_ns);
        auto watchdog = stage_watchdog_deadline(budget, dispatch_now);
        if (!watchdog && participant.payload_kind == OperationParticipantPayloadKind::kMotion)
          watchdog = hard_stage_watchdog_deadline(
              command, participant.stop_readiness->value().hard_stop_completion_timeout_ns,
              dispatch_now);
        if (budget.remaining_controlled_deadline_ns.has_value() &&
            *budget.remaining_controlled_deadline_ns <= 0) {
          retain_first_error(error(std::string(to_string(ErrorCode::kExecutionTimeout)),
                                   "Controlled Stop deadline passed before its dispatch",
                                   "participant_stop_stage_deadline_expired"));
          advance_after_failed_stage();
          continue;
        }
        if (participant.stop_execution_port == nullptr) {
          retain_first_error(error(std::string(to_string(ErrorCode::kInvariantViolation)),
                                   "prepared Stop participant has no execution port",
                                   "participant_stop_execution_port_missing"));
          advance_after_failed_stage();
          continue;
        }
        auto stage = resolve_participant_stop_stage(participant, operation_id, controller_goal_id,
                                                    item.current_stage_index, budget);
        if (!stage.ok()) {
          retain_first_error(stage.error());
          advance_after_failed_stage();
          continue;
        }
        attempt.stage =
            std::make_shared<const ResolvedParticipantStopStageDTO>(std::move(stage.value()));
        attempt.budget = std::make_shared<const RemainingStopBudgetDTO>(std::move(budget));
        const auto started = stop_stage_attempt_executor_->TryStart(
            StopStageAttemptRequest{participant.stop_execution_port, attempt.stage, attempt.budget,
                                    item.force_spring_return && item.current_stage_index == 0U,
                                    &clock_, item.accepted_at_ns});
        if (started.status != StopStageAttemptStartStatus::kStarted ||
            !started.handle.has_value()) {
          item.terminal = true;
          item.contained = false;
          item.terminal_error =
              error(std::string(to_string(ErrorCode::kInvariantViolation)),
                    "prequalified Stop-stage isolation could not accept prepared work",
                    "stop_stage_attempt_prequalification_failed");
          break;
        }
        attempt.started = true;
        attempt.handle = *started.handle;
        attempt.stage_deadline_ns = watchdog;
        if (watchdog.has_value())
          next_deadline = !next_deadline ? watchdog : std::optional(std::min(*next_deadline, *watchdog));
        break;
      }

      if (item.terminal && !item.terminal_projection_complete) {
        auto& participant_state = operation.state.participants[ordinal];
        const bool participant_settlement_required = !item.settlement_not_required();
        bool settlement_waits = false;
        if (item.contained && participant_settlement_required &&
            !item.participant_settlement_complete) {
          // A gripper cancel is fire-and-forget: its exact local settlement
          // is handed to the still-returning dispatch.
          const bool defer_settlement =
              item.payload_kind == OperationParticipantPayloadKind::kCapabilityCommand &&
              participant.dispatch_effect_in_flight;
          // A motion whose committed send was still in flight at this Stop's
          // fence: the settlement waits until the participant reports
          // whether the goal went out (its report wakes this lane). Not sent
          // because this Stop came first: it settles as a pre-dispatch
          // cancellation (no effect); sent: as a dispatched motion.
          const bool motion_send_outcome_awaited =
              item.payload_kind == OperationParticipantPayloadKind::kMotion &&
              item.prepared_execution_id != 0U &&
              participant.motion_send_outcome_awaited == item.prepared_execution_id;
          if (!motion_send_outcome_awaited && !item.participant_settlement_requested &&
              item.payload_kind == OperationParticipantPayloadKind::kMotion &&
              item.prepared_execution_id != 0U &&
              participant.motion_send_refused_by_stop == item.prepared_execution_id) {
            item.cancelled_pre_dispatch = true;
            participant.stop_cancelled_pre_dispatch = true;
          }
          if (!motion_send_outcome_awaited && !item.participant_settlement_requested) {
            item.participant_settlement_requested = true;
            if (participant.engine == nullptr || item.prepared_execution_id == 0U) {
              item.participant_settlement_complete = true;
              item.participant_settlement_error =
                  error(std::string(to_string(ErrorCode::kInvariantViolation)),
                        "Stopped participant lost its local execution owner",
                        "participant_stop_settlement_authority_missing");
            } else {
              std::optional<ParticipantStopStageResultDTO> settlement_evidence;
              if (item.cancelled_pre_dispatch) {
                auto budget = remaining_stop_budget(plan.stage_commands.front(), stop_event, now,
                                                    item.accepted_at_ns);
                auto stage = resolve_participant_stop_stage(participant, operation_id,
                                                            controller_goal_id, 0U, budget);
                if (stage.ok()) {
                  settlement_evidence = ParticipantStopStageResultDTO{
                      std::move(stage.value()), StopStageAttemptDispositionDTO::kNotDispatched,
                      ParticipantStopStageOutcomeDTO::kSucceeded, std::nullopt, std::nullopt,
                      std::nullopt, std::nullopt, std::nullopt, 0, {}, std::nullopt};
                } else {
                  item.participant_settlement_complete = true;
                  item.participant_settlement_error = stage.error();
                }
              } else {
                const auto containment = std::find_if(
                    item.retained_results.rbegin(), item.retained_results.rend(),
                    [](const auto& candidate) {
                      return candidate.authoritative &&
                             candidate.result.outcome == ParticipantStopStageOutcomeDTO::kSucceeded &&
                             !candidate.result.error.has_value();
                    });
                if (containment != item.retained_results.rend())
                  settlement_evidence = containment->result;
              }
              if (!item.participant_settlement_complete && !settlement_evidence.has_value()) {
                item.participant_settlement_complete = true;
                item.participant_settlement_error =
                    error(std::string(to_string(ErrorCode::kInvariantViolation)),
                          "Contained participant has no exact containment result",
                          "participant_stop_containment_result_missing");
              } else if (settlement_evidence.has_value()) {
                AddressedParticipantStopSettlementRequestDTO settlement_request{
                    item.prepared_execution_id, stop_event, std::move(*settlement_evidence),
                    item.payload_kind, item.cancelled_pre_dispatch};
                if (defer_settlement) {
                  participant.deferred_stop_settlement = std::move(settlement_request);
                  item.participant_settlement_complete = true;
                } else if (item.cancel_terminal_deferred) {
                  participant.deferred_cancel_settlement =
                      ActiveOperationParticipant::DeferredPreDispatchCancelSettlement{
                          std::move(settlement_request), token, std::nullopt, std::nullopt, false};
                  item.participant_settlement_complete = true;
                } else {
                  participant_settlement =
                      ParticipantSettlement{participant.engine, std::move(settlement_request)};
                }
              }
            }
          }
          settlement_waits = !item.participant_settlement_complete;
        }
        if (!settlement_waits) {
          // Terminal projection of the participant (unchanged mapping).
          const auto rules_retired = retire_participant_io_rules_locked(participant);
          if (!rules_retired.ok()) item.participant_settlement_error = rules_retired.error();
          const bool io_source_failed = std::any_of(
              participant_state.io_rule_outcomes.begin(), participant_state.io_rule_outcomes.end(),
              [](const auto& outcome) { return !outcome.evidence.source_losses.empty(); });
          const bool participant_settled =
              item.contained && !item.preparation_failed && !item.stop_path_collision.has_value() &&
              (!participant_settlement_required ||
               (item.participant_settlement_complete && !item.participant_settlement_error.has_value()));
          const bool runtime_collision_participant =
              item.runtime_collision_containment && participant.runtime_collision_error.has_value();
          participant_state.state =
              item.participant_failure_preserved ? OperationStateKind::kFailed
              : participant_settled && !runtime_collision_participant
                  ? (io_source_failed ? OperationStateKind::kFailed : OperationStateKind::kStopped)
                  : OperationStateKind::kInterventionRequired;
          if (item.stop_path_collision.has_value()) {
            participant_state.terminal_error = item.stop_path_collision;
          } else if (!participant_settled && !participant_state.terminal_error.has_value()) {
            participant_state.terminal_error = item.participant_settlement_error.has_value()
                                                   ? item.participant_settlement_error
                                                   : item.terminal_error;
          } else if (io_source_failed && participant_settled && !runtime_collision_participant) {
            participant_state.terminal_error =
                error("interrupt_source_unavailable", "A required motion interrupt source failed",
                      "io_rule_required_source_lost");
          } else if (item.carried_object_lost && participant_settled &&
                     !runtime_collision_participant) {
            participant_state.terminal_error =
                error("carried_object_lost", "The carried object was lost from its grasp",
                      "carried_object_lost");
          } else if (runtime_collision_participant) {
            participant_state.terminal_error = participant.runtime_collision_error;
          } else if (item.runtime_collision_containment &&
                     !participant_state.terminal_error.has_value()) {
            participant_state.terminal_error = participant.last_subordinate_error;
          } else if (participant_settled && item.escalation_cause.has_value() &&
                     !item.internal_control_stop &&
                     !planner_selected_hard_stop(*item.escalation_cause) &&
                     !participant_state.terminal_error.has_value()) {
            // A successful fallback hard stop ends Stopped with `main`'s
            // degraded-Stop error.
            const auto containing = std::find_if(
                item.retained_results.rbegin(), item.retained_results.rend(),
                [](const auto& candidate) {
                  return candidate.authoritative &&
                         candidate.result.outcome == ParticipantStopStageOutcomeDTO::kSucceeded;
                });
            participant_state.terminal_error = degraded_stop_error(
                *item.escalation_cause, plan,
                containing != item.retained_results.rend() ? containing->result.request.stage_index
                                                           : plan.stage_commands.size());
          }
          const bool all_participants_terminal =
              std::all_of(operation.state.participants.begin(), operation.state.participants.end(),
                          [](const auto& candidate) { return terminal(candidate.state); });
          if (all_participants_terminal) {
            const bool has_pneumatic_transaction =
                std::any_of(operation.participants.begin(), operation.participants.end(),
                            [](const auto& row) { return row.finishes_current_gripper_transaction(); });
            operation.state.state =
                (has_pneumatic_transaction || io_source_failed)
                    ? aggregate_participant_state(operation.state.participants)
                : std::all_of(operation.state.participants.begin(),
                              operation.state.participants.end(),
                              [](const auto& candidate) {
                                return candidate.state == OperationStateKind::kStopped;
                              })
                    ? OperationStateKind::kStopped
                    : OperationStateKind::kInterventionRequired;
          } else {
            // Once the moving rows have settled, ordinary siblings keep their
            // truthful aggregate state independently of Stop.
            bool moving_settled = true;
            for (std::size_t index = 0U; index < operation.participants.size(); ++index) {
              if (!operation.participants[index].stop_owns_reconciliation()) continue;
              if (index >= operation.state.participants.size() ||
                  !terminal(operation.state.participants[index].state))
                moving_settled = false;
            }
            if (moving_settled)
              operation.state.state = aggregate_participant_state(operation.state.participants);
          }
          operation.state.updated_at_ns = now;
          // P4 (3): the participant's slots go from Stopping to Terminal and
          // stay owned (a backend terminal callback may already have done it).
          if (participant.slot_state != ResourceSlotState::kTerminal) {
            const auto transitioned = resource_slots_->transition(
                participant.slot_owner, participant.resources, ResourceSlotState::kTerminal);
            if (transitioned.ok()) {
              participant.slot_state = ResourceSlotState::kTerminal;
            } else {
              operation.state.state = OperationStateKind::kInterventionRequired;
              participant_state.state = OperationStateKind::kInterventionRequired;
              participant_state.terminal_error = transitioned.error();
            }
          }
          if (item.payload_kind == OperationParticipantPayloadKind::kMotion) {
            if (item.preparation_failed) {
              if (!participant_state.terminal_fact)
                participant_state.terminal_fact = ParticipantExecutionFact{
                    ParticipantExecutionFactKind::kLocalFailure,
                    ParticipantEffectKnowledge::kNoEffect, participant_state.terminal_error};
            } else if (participant_settled && !runtime_collision_participant &&
                       !item.participant_failure_preserved) {
              participant_state.terminal_fact = ParticipantExecutionFact{
                  item.cancelled_pre_dispatch
                      ? ParticipantExecutionFactKind::kCancelledBeforeDispatch
                      : ParticipantExecutionFactKind::kPreemptedByAddressedStop,
                  item.cancelled_pre_dispatch ? ParticipantEffectKnowledge::kNoEffect
                                              : ParticipantEffectKnowledge::kEffectSettled,
                  participant_state.terminal_error};
            }
          }
          item.terminal_projection_complete = true;
        }
      }

      const bool attempt_ended =
          item.terminal && item.terminal_projection_complete &&
          (!item.contained || item.settlement_not_required() || item.participant_settlement_complete);
      item.progress_in_flight = false;
      if (!attempt_ended) {
        if (next_deadline.has_value()) stop_control_executor_->ArmDeadline(token, *next_deadline);
      } else {
        // P4/P5 (may raise F1's Halt into the hand-off and restart this
        // participant: nothing below touches its work).
        complete_participant_stop_locked(operation, ordinal, now, handoff);
        const bool operation_stopping =
            std::any_of(operation.participants.begin(), operation.participants.end(),
                        [](const auto& row) { return row.stop.phase == StopPhase::kStopping; });
        // The participant's outcome, or the operation's terminal state.
        capture_stop_operation_state_locked(
            operation, operation_stopping ? "stop_participant_completed" : "stop_terminal_committed");
        handoff.journal_pending = true;
        if (!operation_stopping) {
          operation_settled = operation_id;
          // P4 (3): settle a pending control (unchanged).
          if (operation.pending_control.has_value() && !operation.has_pending_io_rule_response()) {
            auto& pending = *operation.pending_control;
            if (pending.accepted.control != OperationControlKind::kReset) {
              if (pending.accepted.control == OperationControlKind::kAcknowledgeCompleted &&
                  operation.ordinary_pneumatic_acknowledgement() &&
                  !pending.first_failure.has_value()) {
                settle_completed_control_locked(operation_id);
              } else {
                if (pending.first_failure.has_value())
                  static_cast<void>(capture_control_failure_locked(operation));
                if (pending.completed_effects == pending.expected_effects)
                  operation.pending_control.reset();
              }
            } else if (pending.effects_dispatched) {
              if (pending.completed_effects == pending.expected_effects) {
                if (pending.first_failure.has_value()) {
                  static_cast<void>(publish_settlement_fault_locked(operation));
                } else {
                  (void)settle_control_operation_locked(operation_id, OperationControlKind::kReset);
                }
              }
            } else {
              auto requests = reset_effect_requests(
                  operation_id, operation.participants,
                  pending.accepted.operation_revision_after_acceptance);
              if (!requests.ok() || pending.completed_effects != 0U) {
                operation.state.state = OperationStateKind::kInterventionRequired;
                operation.state.updated_at_ns = now;
                operation.pending_control.reset();
              } else {
                pending.superseded_by_stop = false;
                pending.expected_effects = requests.value().size();
                pending.effects_dispatched = true;
                if (requests.value().empty()) {
                  (void)settle_control_operation_locked(operation_id, OperationControlKind::kReset);
                } else {
                  reset_effects = std::move(requests.value());
                }
              }
            }
          }
        }
      }
    }
    complete_stop_handoff(handoff);

    if (participant_settlement.has_value()) {
      const auto& handoff_request = participant_settlement->request;
      Result<AddressedParticipantStopSettlementDTO> settled =
          Result<AddressedParticipantStopSettlementDTO>::Err(
              error(std::string(to_string(ErrorCode::kInternalError)),
                    "addressed motion Stop settlement did not return a result",
                    "participant_stop_settlement_failed"));
      try {
        settled = participant_settlement->engine->settle_addressed_participant_stop(handoff_request);
      } catch (const std::exception& exception) {
        settled = Result<AddressedParticipantStopSettlementDTO>::Err(
            error(std::string(to_string(ErrorCode::kInvariantViolation)),
                  std::string("addressed motion Stop settlement threw: ") + exception.what(),
                  "participant_stop_settlement_threw"));
      } catch (...) {
        settled = Result<AddressedParticipantStopSettlementDTO>::Err(
            error(std::string(to_string(ErrorCode::kInvariantViolation)),
                  "addressed motion Stop settlement threw", "participant_stop_settlement_threw"));
      }
      if (settled.ok()) {
        const auto& value = settled.value();
        const auto& stage = handoff_request.containment.request;
        const auto& recovery = value.recovery_assessment;
        const bool failed_contained =
            value.terminal_fact && is_settled_execution_failure(*value.terminal_fact) &&
            handoff_request.payload_kind == OperationParticipantPayloadKind::kMotion &&
            failed_not_retry_admissible(recovery);
        const bool ordinary_stopped =
            !value.terminal_fact && recovery.execution_state == ExecutionState::kStopped &&
            recovery.recovery_class == RecoveryClass::kOrdinaryOperatorStop &&
            ((recovery.status == RecoveryAssessmentStatus::kRetryReady && recovery.retry_admissible) ||
             stopped_awaiting_terminal_cleanup(recovery));
        const bool exact = value.execution_id == handoff_request.execution_id &&
                           value.stop_event_id == handoff_request.stop_event_id &&
                           value.operation_id == stage.operation_id &&
                           value.participant_id == stage.participant_id &&
                           value.controller_goal_id == stage.controller_goal_id &&
                           recovery.execution_id == handoff_request.execution_id &&
                           (ordinary_stopped || failed_contained) &&
                           recovery.backend_stop_confirmed &&
                           recovery.stop_event_id == handoff_request.stop_event_id.value();
        if (!exact)
          settled = Result<AddressedParticipantStopSettlementDTO>::Err(
              error(std::string(to_string(ErrorCode::kInvariantViolation)),
                    "participant returned a mismatched addressed Stop settlement",
                    "participant_stop_settlement_mismatch"));
      }
      std::lock_guard state_lock(state_mutex_);
      const auto found = participant_for_token();
      auto* const operation_ptr = found.first;
      const auto ordinal = found.second;
      if (operation_ptr != nullptr) {
        auto& item = operation_ptr->participants[ordinal].stop_work;
        if (item.prepared_execution_id == handoff_request.execution_id &&
            item.participant_settlement_requested) {
          item.participant_settlement_complete = true;
          if (settled.ok() && settled.value().terminal_fact) {
            auto& state = operation_ptr->state.participants[ordinal];
            const auto& fact = *settled.value().terminal_fact;
            if ((state.terminal_fact && *state.terminal_fact != fact) ||
                (state.terminal_error && state.terminal_error != fact.error)) {
              settled = Result<AddressedParticipantStopSettlementDTO>::Err(
                  error(std::string(to_string(ErrorCode::kInvariantViolation)),
                        "participant returned a different contained failure",
                        "participant_stop_settlement_mismatch"));
            } else {
              state.terminal_fact = fact;
              state.terminal_error = fact.error;
              item.participant_failure_preserved = true;
            }
          }
          // Operation-scoped: the participant ends InterventionRequired and
          // its resources stay owned until Reset; the cell stays admissible.
          if (!settled.ok()) item.participant_settlement_error = settled.error();
        }
      }
    }
    if (prepared_cancellation.has_value()) {
      const auto& cancellation = *prepared_cancellation;
      std::optional<ExecutionStatus> current;
      Result<void> cancelled = Result<void>::Err(error(
          std::string(to_string(ErrorCode::kInternalError)),
          "prepared motion cancellation did not return a result", "prepared_motion_cancel_failed"));
      try {
        cancelled = cancellation.payload_kind == OperationParticipantPayloadKind::kCapabilityCommand
                        ? cancellation.engine->CancelPreparedCapabilityCommand(cancellation.execution_id)
                        : cancellation.engine->CancelPreparedMotion(cancellation.execution_id);
        if (cancelled.ok() && cancellation.payload_kind == OperationParticipantPayloadKind::kMotion)
          current = cancellation.execution_engine->get_status();
      } catch (const std::exception& exception) {
        cancelled = Result<void>::Err(
            error(std::string(to_string(ErrorCode::kInvariantViolation)),
                  std::string("prepared motion cancellation threw: ") + exception.what(),
                  "prepared_motion_cancel_threw"));
      } catch (...) {
        cancelled = Result<void>::Err(error(std::string(to_string(ErrorCode::kInvariantViolation)),
                                            "prepared motion cancellation threw",
                                            "prepared_motion_cancel_threw"));
      }
      std::lock_guard state_lock(state_mutex_);
      const auto found = participant_for_token();
      auto* const operation_ptr = found.first;
      const auto ordinal = found.second;
      if (operation_ptr != nullptr) {
        auto& item = operation_ptr->participants[ordinal].stop_work;
        if (item.prepared_execution_id == cancellation.execution_id) {
          item.prepared_cancel_complete = true;
          if (!cancelled.ok()) {
            fail_prepared_cancellation(item, cancelled.error());
          } else if (item.payload_kind == OperationParticipantPayloadKind::kCapabilityCommand) {
            // The successful cancellation call is the capability command's
            // complete local no-effect transition.
            item.prepared_cancel_terminal_observed = true;
          } else if (current.has_value()) {
            observe_prepared_cancellation_terminal(
                item, current->execution_id, participant_operation_state(*current),
                participant_fact_error(*current).value_or(status_error(current->last_error)));
          }
        }
      }
    }
    if (prepared_classification.has_value()) {
      const auto& classification = *prepared_classification;
      std::optional<ResolvedPreparedMotionExecutionSnapshotDTO> prepared;
      try {
        auto snapshot = classification.engine->ObservePreparedMotion(classification.execution_id);
        if (snapshot.ok()) prepared = std::move(snapshot.value());
      } catch (...) {
        prepared.reset();
      }
      std::lock_guard state_lock(state_mutex_);
      const auto found = participant_for_token();
      auto* const operation_ptr = found.first;
      const auto ordinal = found.second;
      if (operation_ptr != nullptr) {
        auto& item = operation_ptr->participants[ordinal].stop_work;
        if (item.prepared_execution_id == classification.execution_id &&
            item.unclassified_prepared_terminal.has_value()) {
          if (prepared.has_value() && prepared->execution_id == classification.execution_id &&
              prepared->state == ResolvedPreparedMotionExecutionStateDTO::kPreparationFailed &&
              !prepared->last_dispatched_participant) {
            settle_failed_preparation(item, prepared->error.value_or(*item.unclassified_prepared_terminal));
          } else {
            fail_prepared_cancellation(item, *item.unclassified_prepared_terminal);
          }
        }
      }
    }
    if (prepared_cancellation || prepared_classification || participant_settlement) {
      // The executor coalesces identical publications: finish the bounded,
      // effect-free pass directly so this lane commits its outcome now.
      ProgressStop(token);
      return;
    }
    for (auto& effect : reset_effects) {
      const auto identity = effect;
      auto started = operation_control_executor_->TryStart(std::move(effect));
      if (started.status == OperationControlEffectStartStatus::kStarted) continue;
      CompleteOperationControlEffect(OperationControlEffectResult{
          identity.operation_id, identity.participant_id, identity.operation_revision,
          identity.control, false,
          error(std::string(to_string(ErrorCode::kInvariantViolation)),
                "prequalified operation-control lane rejected certified Reset work",
                "operation_control_effect_handoff_failed")});
    }
    if (operation_settled.has_value()) {
      // Reevaluate idle global rules once at this actual terminal boundary
      // (unchanged), then complete their Stops' hand-off.
      {
        std::lock_guard lock(state_mutex_);
        if (global_io_rule_catalog_ && global_io_rule_service_->EvaluateRules().ok())
          static_cast<void>(process_global_io_rule_evidence_locked(handoff));
      }
      complete_stop_handoff(handoff);
    }
  } catch (...) {
    // Only a defect throws here. A half-made pass changes the lane's state
    // only by `progress_in_flight`, cleared below; then the cell is halted.
    try {
      std::lock_guard state_lock(state_mutex_);
      if (token.locally_valid() && token.slot < stop_lanes_.owners.size()) {
        const auto& owner = stop_lanes_.owners[token.slot];
        if (owner.generation == token.generation && owner.operation_id.nonzero())
          if (auto* operation = find_active_operation_locked(owner.operation_id);
              operation != nullptr && owner.participant_ordinal < operation->participants.size())
            operation->participants[owner.participant_ordinal].stop_work.progress_in_flight = false;
      }
    } catch (...) {
    }
    ContainInternalFault(error_codes::kStopProgressException);
  }
}

// ---------------------------------------------------------------------------
// Journal writer thread: the Stop rows of one taken batch.
// ---------------------------------------------------------------------------

std::uint64_t ResourceAddressedExecutionKernel::copy_journal_stop_rows(
    std::span<JournalRecord> taken) noexcept {
  std::uint64_t lost = 0U;
  const JournalRecord* first_lost = nullptr;
  const auto lanes = prequalified_max_live_stop_participants_;
  for (std::size_t index = 0U; index < taken.size(); ++index) {
    auto* row = std::get_if<JournalStopEventRow>(&taken[index].payload);
    if (row == nullptr) continue;
    // Sized like the ring's records here, so the copy under the lock allocates nothing.
    auto copy = std::make_shared<StopEventRecord>();
    copy->covered.resize(lanes);
    copy->stale.resize(lanes);
    copy->covered_lanes.resize((lanes + 63U) / 64U);
    bool overwritten = false;
    {
      std::lock_guard state_lock(state_mutex_);
      const auto& record = stop_events_.records[row->ring_slot];
      overwritten = record.event_id != row->event_id;
      if (!overwritten) *copy = record;
    }
    if (overwritten) {
      // Its ring slot was reused before the copy: the row is not written.
      taken[index].skipped = true;
      ++lost;
      if (first_lost == nullptr) first_lost = &taken[index];
      continue;
    }
    if (!row->completed && copy->fence_degraded) {
      const auto reason = stop_error_code_text(copy->fence_degraded_reason);
      (void)std::fprintf(stderr,
                         "manyforge_fault code=stop_fence_degraded event_id=%llu reason=%.*s\n",
                         static_cast<unsigned long long>(copy->event_id.value()),
                         static_cast<int>(reason ? reason->size() : 0U),
                         reason ? reason->data() : "");
    }
    row->record = std::move(copy);
  }
  if (lost != 0U) {
    std::lock_guard state_lock(state_mutex_);
    note_journal_records_lost_locked(*first_lost, lost, journal_records_lost_error());
  }
  return lost;
}

}  // namespace manyforge::core
