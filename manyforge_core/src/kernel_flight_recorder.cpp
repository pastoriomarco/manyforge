#include <manyforge_core/kernel_flight_recorder.hpp>

#include <algorithm>
#include <chrono>
#include <cinttypes>

#include <manyforge_core/types/execution_status.hpp>

namespace manyforge::core {

namespace {

// Line-atomic write helper: one snprintf into a stack buffer, one fwrite.
// Keeps the output structurally valid even if the process dies mid-dump
// (spec 520 §4.2: a torn staging tail is acceptable, corrupt records are
// not).
void write_line(std::FILE* out, const char* buffer, int length) noexcept {
  if (length <= 0) {
    return;
  }
  (void)std::fwrite(buffer, 1, static_cast<std::size_t>(length), out);
  (void)std::fwrite("\n", 1, 1, out);
}

}  // namespace

const char* kernel_event_type_name(KernelEventType type) {
  switch (type) {
    using enum KernelEventType;
    case kStateTransition: return "state_transition";
    case kPlanStarted: return "plan_started";
    case kPlanCompleted: return "plan_completed";
    case kCacheHit: return "cache_hit";
    case kCacheMiss: return "cache_miss";
    case kCacheInvalidated: return "cache_invalidated";
    case kValidationStarted: return "validation_started";
    case kValidationPassed: return "validation_passed";
    case kValidationFailed: return "validation_failed";
    case kExecuteStarted: return "execute_started";
    case kExecuteCompleted: return "execute_completed";
    case kStopRequested: return "stop_requested";
    case kStopIssued: return "stop_issued";
    case kPauseRequested: return "pause_requested";
    case kResumeRequested: return "resume_requested";
    case kRuntimeVelocityScaleChanged: return "runtime_velocity_scale_changed";
    case kCompletedAcknowledged: return "completed_acknowledged";
    case kFailedValidationAcknowledged:
      return "failed_validation_acknowledged";
    case kInterventionAcknowledged: return "intervention_acknowledged";
    case kErrorRaised: return "error_raised";
    case kCollisionDetected: return "collision_detected";
    case kInterruptSignalIngested: return "interrupt_signal_ingested";
    case kInterruptSignalRejectedStale: return "interrupt_signal_rejected_stale";
    case kInterruptSignalRejectedClassViolation:
      return "interrupt_signal_rejected_class_violation";
    case kInterruptSignalRejected: return "interrupt_signal_rejected";
    case kInterruptLatched: return "interrupt_latched";
    case kInterruptSuppressedByActiveMask:
      return "interrupt_suppressed_by_active_mask";
    case kInterruptQueued: return "interrupt_queued";
    case kInterruptDroppedQueueOverflow:
      return "interrupt_dropped_queue_overflow";
    case kInterruptPreemptApplied: return "interrupt_preempt_applied";
    case kInterruptDispositionApplied: return "interrupt_disposition_applied";
    case kInterruptCleared: return "interrupt_cleared";
    case kInterruptIngestOverflow: return "interrupt_ingest_overflow";
    case kInterruptSourceClassViolation:
      return "interrupt_source_class_violation";
    case kInterruptRecoveryHandlerStarted:
      return "interrupt_recovery_handler_started";
    case kInterruptRecoveryHandlerCompleted:
      return "interrupt_recovery_handler_completed";
    case kMotionInterruptScopeArmed: return "motion_interrupt_scope_armed";
    case kMotionInterruptScopeReaped: return "motion_interrupt_scope_reaped";
    case kMotionInterruptArmFailed: return "motion_interrupt_arm_failed";
    case kMotionInterruptQueuedUpdateDropped:
      return "motion_interrupt_queued_update_dropped";
    case kMotionTripSecondarySignal: return "motion_trip_secondary_signal";
    case kMotionTripFailed: return "motion_trip_failed";
    case kMotionTripCompleted: return "motion_trip_completed";
    case kMotionExecutionOutcomeRecorded:
      return "motion_execution_outcome_recorded";
    case kStopCertificatePublished: return "stop_certificate_published";
    case kRecoveryAssessmentChanged: return "recovery_assessment_changed";
    case kRecoveryActionAccepted: return "recovery_action_accepted";
    case kRecoveryActionRejected: return "recovery_action_rejected";
    case kRuntimeCollisionCoverage: return "runtime_collision_coverage";
  }
  return "unknown";
}

void KernelFlightRecorder::record(const KernelEvent& event) noexcept {
  std::lock_guard<std::mutex> lock(mutex_);
  // Use the kernel-assigned dispatch sequence when present so flight records
  // correlate 1:1 with the downstream pipeline; fall back to a local counter
  // for direct/unit callers. Both streams are contiguous, which the ring
  // slot math and dump iteration rely on.
  const std::uint64_t sequence =
      event.sequence != 0 ? event.sequence : next_sequence_;
  // Kernel-assigned sequences can arrive out of dispatch order (the atomic
  // is fetched before the recorder mutex); never let a straggler regress the
  // high-water mark, or recorded_total()/dump iteration would undercount and
  // omit already-recorded events.
  next_sequence_ = std::max(next_sequence_, sequence + 1);
  FlightRecord& slot = ring_[(sequence - 1) % kCapacity];
  slot.sequence = sequence;
  slot.timestamp_ns = event.timestamp_ns;
  slot.type = event.type;
  slot.execution_id = event.execution_id;
  slot.from_state = event.from_state;
  slot.to_state = event.to_state;
  if (event.error.has_value()) {
    (void)std::snprintf(slot.error_code, sizeof(slot.error_code), "%s",
                        event.error->code.c_str());
  } else {
    slot.error_code[0] = '\0';
  }
}

void KernelFlightRecorder::note_published_snapshot(
    std::uint64_t sequence_number, std::int64_t timestamp_ns,
    std::uint64_t execution_id, ExecutionState state) noexcept {
  std::lock_guard<std::mutex> lock(mutex_);
  snapshot_note_.sequence_number = sequence_number;
  snapshot_note_.timestamp_ns = timestamp_ns;
  snapshot_note_.execution_id = execution_id;
  snapshot_note_.state = state;
  snapshot_note_.present = true;
}

void KernelFlightRecorder::set_dump_directory(std::string directory) {
  std::lock_guard<std::mutex> lock(mutex_);
  dump_directory_ = std::move(directory);
}

std::uint64_t KernelFlightRecorder::recorded_total() const noexcept {
  std::lock_guard<std::mutex> lock(mutex_);
  return next_sequence_ - 1;
}

std::string KernelFlightRecorder::dump_now(const char* reason) noexcept {
  // Crash-context lock acquisition: spin briefly, then emit only a minimal
  // marker if the lock remains busy. A dying kernel must not deadlock behind a
  // telemetry mutex, but unsynchronized ring reads would be undefined behavior.
  std::array<FlightRecord, kCapacity> copy;
  std::uint64_t next_sequence = 0;
  SnapshotNote snapshot_note;
  std::string directory;
  bool torn = false;
  {
    bool locked = false;
    for (int attempt = 0; attempt < 4096 && !locked; ++attempt) {
      locked = mutex_.try_lock();
    }
    if (locked) {
      copy = ring_;
      next_sequence = next_sequence_;
      snapshot_note = snapshot_note_;
      directory = dump_directory_;
      mutex_.unlock();
    } else {
      // Reading the ring without synchronization is a C++ data race, not a
      // valid "torn" snapshot. In a crash-context lock collision, emit a
      // minimal torn marker to stderr and preserve process progress without
      // touching concurrently-written storage.
      torn = true;
      next_sequence = 1;
      directory.clear();
    }
  }

  std::FILE* out = nullptr;
  std::string path;
  if (!directory.empty()) {
    const auto wall_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count();
    char name[64];
    (void)std::snprintf(name, sizeof(name), "/flight-%lld.jsonl",
                        static_cast<long long>(wall_ms));
    path = directory + name;
    out = std::fopen(path.c_str(), "w");
  }
  const bool to_stderr = out == nullptr;
  if (to_stderr) {
    path.clear();
    out = stderr;
    (void)std::fputs("MANYFORGE_KERNEL_FLIGHT_DUMP_BEGIN\n", stderr);
  }

  char line[512];
  const auto wall_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                           std::chrono::system_clock::now().time_since_epoch())
                           .count();
  const std::uint64_t total = next_sequence == 0 ? 0 : next_sequence - 1;
  const std::uint64_t count =
      total > kCapacity ? static_cast<std::uint64_t>(kCapacity) : total;
  int n = std::snprintf(
      line, sizeof(line),
      "{\"schema_version\":\"manyforge.kernel_flight.v1\",\"reason\":\"%s\","
      "\"dumped_wall_ns\":%lld,\"records\":%" PRIu64
      ",\"recorded_total\":%" PRIu64 ",\"torn\":%s}",
      reason == nullptr ? "unspecified" : reason,
      static_cast<long long>(wall_ns), count, total, torn ? "true" : "false");
  write_line(out, line, n);

  // Oldest-first: the ring slot for sequence s is (s-1) % kCapacity. A slot
  // whose stored sequence disagrees (torn copy) is skipped, keeping the
  // output line-valid.
  for (std::uint64_t s = total - count + 1; s <= total && count != 0; ++s) {
    const FlightRecord& r = copy[(s - 1) % kCapacity];
    if (r.sequence != s) {
      continue;
    }
    n = std::snprintf(
        line, sizeof(line),
        "{\"seq\":%" PRIu64 ",\"ts_ns\":%lld,\"type\":\"%s\","
        "\"execution_id\":%" PRIu64 ",\"from\":\"%s\",\"to\":\"%s\"%s%s%s}",
        r.sequence, static_cast<long long>(r.timestamp_ns),
        kernel_event_type_name(r.type), r.execution_id,
        to_string(r.from_state), to_string(r.to_state),
        r.error_code[0] != '\0' ? ",\"error_code\":\"" : "",
        r.error_code[0] != '\0' ? r.error_code : "",
        r.error_code[0] != '\0' ? "\"" : "");
    write_line(out, line, n);
  }

  if (snapshot_note.present) {
    n = std::snprintf(
        line, sizeof(line),
        "{\"snapshot\":{\"sequence_number\":%" PRIu64 ",\"ts_ns\":%lld,"
        "\"execution_id\":%" PRIu64 ",\"state\":\"%s\"}}",
        snapshot_note.sequence_number,
        static_cast<long long>(snapshot_note.timestamp_ns),
        snapshot_note.execution_id, to_string(snapshot_note.state));
    write_line(out, line, n);
  }

  if (to_stderr) {
    (void)std::fputs("MANYFORGE_KERNEL_FLIGHT_DUMP_END\n", stderr);
    (void)std::fflush(stderr);
  } else {
    (void)std::fflush(out);
    (void)std::fclose(out);
  }
  return path;
}

}  // namespace manyforge::core
