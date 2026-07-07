#pragma once

#include <manyforge_core/types/execution_result.hpp>

#include <cstdint>
#include <mutex>

namespace manyforge::core {

/// Owns observer-facing execution snapshot storage and sequence assignment.
///
/// ExecutionKernel remains responsible for lifecycle policy, correlation
/// stamping, and live backend reads. This collaborator only owns the published
/// snapshot cache so callers cannot reach through to its mutex or sequence
/// counter.
class ExecutionSnapshotPublisher {
 public:
  /// Compact identity of a published snapshot, for callers that need the
  /// assigned sequence without copying the full snapshot back out.
  struct PublishedIdentity {
    std::uint64_t sequence_number = 0;
    std::int64_t timestamp_ns = 0;
    std::uint64_t execution_id = 0;
    ExecutionState state{};
  };

  PublishedIdentity publish(ExecutionSnapshot snapshot,
                            std::int64_t fallback_timestamp_ns);

  ExecutionSnapshot latest() const;

 private:
  mutable std::mutex mutex_;
  ExecutionSnapshot published_snapshot_;
  std::uint64_t next_sequence_number_ = 1;
};

}  // namespace manyforge::core
