#pragma once

#include <array>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>

#include <manyforge_core/types/execution_result.hpp>
#include <manyforge_core/types/kernel_event.hpp>

namespace manyforge::core {

/// One kernel event, compacted to a fixed-size, allocation-free record for
/// the fail-closed flight ring. Only the fields needed to reconstruct the
/// decision timeline survive; free-text details stay in the full event
/// pipeline. `error_code` is a truncated copy of `ErrorDTO::code` when the
/// event carried an error.
struct FlightRecord {
  std::uint64_t sequence = 0;
  std::int64_t timestamp_ns = 0;
  KernelEventType type{};
  std::uint64_t execution_id = 0;
  ExecutionState from_state{};
  ExecutionState to_state{};
  char error_code[24] = {};
};

/// Bounded in-process flight ring of recent kernel events plus the latest
/// published snapshot identity, dumped as JSONL when the kernel dies
/// fail-closed. Telemetry only: recording is a fixed-size struct copy under
/// a leaf mutex that is never held across I/O or callbacks; recording
/// failure can never affect control flow (all entry points are noexcept and
/// best-effort). Dumping is crash-context aware: it spins briefly for the
/// ring lock and, failing that, emits a minimal `torn=true` marker without
/// reading concurrently-written storage. The output stays line-valid without
/// relying on undefined data-race behavior.
class KernelFlightRecorder {
 public:
  static constexpr std::size_t kCapacity = 512;

  KernelFlightRecorder() = default;
  KernelFlightRecorder(const KernelFlightRecorder&) = delete;
  KernelFlightRecorder& operator=(const KernelFlightRecorder&) = delete;

  /// Append one event to the ring (drop-oldest). Fixed cost, noexcept.
  void record(const KernelEvent& event) noexcept;

  /// Remember the latest published snapshot's identity (state, execution,
  /// sequence, timestamp) so a dump shows where the observers stood.
  void note_published_snapshot(std::uint64_t sequence_number,
                               std::int64_t timestamp_ns,
                               std::uint64_t execution_id,
                               ExecutionState state) noexcept;

  /// Configure the dump directory. Call once at startup wiring time (not
  /// concurrency-hardened against dump_now; setting it while the kernel is
  /// dying is not a supported sequence). Empty (default) dumps to stderr.
  void set_dump_directory(std::string directory);

  /// Best-effort JSONL dump for fail-closed termination paths. Writes
  /// `<dir>/flight-<wall_ms>.jsonl` and returns the path, or writes to
  /// stderr between BEGIN/END markers and returns empty when no directory
  /// is configured or the file cannot be opened. Never throws.
  std::string dump_now(const char* reason) noexcept;

  /// Total events recorded (monotonic; ring keeps the last kCapacity).
  std::uint64_t recorded_total() const noexcept;

 private:
  mutable std::mutex mutex_;
  std::array<FlightRecord, kCapacity> ring_{};
  std::uint64_t next_sequence_ = 1;  // ring slot = (sequence - 1) % kCapacity
  struct SnapshotNote {
    std::uint64_t sequence_number = 0;
    std::int64_t timestamp_ns = 0;
    std::uint64_t execution_id = 0;
    ExecutionState state{};
    bool present = false;
  } snapshot_note_{};
  std::string dump_directory_;
};

/// Stable lower-snake name for a kernel event type (dump-time only; returns
/// "unknown_<n>" for values without a mapping so new enum entries degrade
/// loudly instead of aborting a dump).
const char* kernel_event_type_name(KernelEventType type);

}  // namespace manyforge::core
