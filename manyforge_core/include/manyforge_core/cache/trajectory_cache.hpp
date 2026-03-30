#pragma once

#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

#include <manyforge_core/cache/cache_policy.hpp>
#include <manyforge_core/types/trajectory_dto.hpp>

namespace manyforge::core {

/// Trajectory cache keyed by move_id (spec 230 §3.1).
/// One trajectory per move_id. No LRU, no TTL.
/// Thread-safe.
class TrajectoryCache {
 public:
  explicit TrajectoryCache(CacheConfig config = {});

  /// Store trajectory for a move_id (overwrites existing).
  void store(const std::string& move_id, TrajectoryDTO trajectory);

  /// Look up trajectory by move_id.
  std::optional<TrajectoryDTO> lookup(const std::string& move_id) const;

  /// Invalidate a specific move_id entry.
  void invalidate(const std::string& move_id);

  /// Remove all entries.
  void invalidate_all();

  /// Number of cached entries.
  size_t size() const;

  /// Max entries (safety bound).
  size_t capacity() const;

 private:
  CacheConfig config_;
  mutable std::mutex mutex_;
  std::unordered_map<std::string, TrajectoryDTO> entries_;
  std::deque<std::string> insertion_order_;  // for FIFO eviction
};

}  // namespace manyforge::core
