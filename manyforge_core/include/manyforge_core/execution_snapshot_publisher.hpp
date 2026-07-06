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
  void publish(ExecutionSnapshot snapshot, std::int64_t fallback_timestamp_ns);

  ExecutionSnapshot latest() const;

 private:
  mutable std::mutex mutex_;
  ExecutionSnapshot published_snapshot_;
  std::uint64_t next_sequence_number_ = 1;
};

}  // namespace manyforge::core
