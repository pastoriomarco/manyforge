#include <manyforge_core/execution_snapshot_publisher.hpp>

#include <utility>

namespace manyforge::core {

ExecutionSnapshotPublisher::PublishedIdentity ExecutionSnapshotPublisher::publish(
    ExecutionSnapshot snapshot, std::int64_t fallback_timestamp_ns) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (snapshot.timestamp_ns == 0) {
    snapshot.timestamp_ns = fallback_timestamp_ns;
  }
  snapshot.sequence_number = next_sequence_number_++;
  published_snapshot_ = std::move(snapshot);
  return PublishedIdentity{published_snapshot_.sequence_number,
                           published_snapshot_.timestamp_ns,
                           published_snapshot_.execution_id,
                           published_snapshot_.state};
}

ExecutionSnapshot ExecutionSnapshotPublisher::latest() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return published_snapshot_;
}

}  // namespace manyforge::core
