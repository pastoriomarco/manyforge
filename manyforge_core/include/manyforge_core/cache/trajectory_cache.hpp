#pragma once

#include <list>
#include <mutex>
#include <unordered_map>

#include <manyforge_core/cache/cache_policy.hpp>
#include <manyforge_core/time/iclock.hpp>
#include <manyforge_core/types/error_code.hpp>
#include <manyforge_core/types/trajectory_dto.hpp>

namespace manyforge::core {

/// Bounded, scene-aware trajectory cache with LRU eviction and TTL.
/// ADR-0008: deterministic invalidation on scene change, collision, or stop.
class TrajectoryCache {
 public:
  explicit TrajectoryCache(IClock& clock, CacheConfig config = {});

  /// Look up a cached trajectory.
  /// @return Ok(trajectory) on hit, Err(kCacheMiss) on miss or expiry.
  Result<TrajectoryDTO> lookup(const CacheKey& key);

  /// Store a trajectory in the cache.
  /// Evicts LRU entry if at capacity.
  void store(const CacheKey& key, TrajectoryDTO trajectory);

  /// Remove all entries with scene_version != current_version.
  /// ADR-0008: scene version change invalidates stale entries.
  void invalidate_for_scene(uint64_t current_scene_version);

  /// Remove all entries. Called on stop/collision.
  void invalidate_all();

  /// Current number of entries.
  size_t size() const;

  /// Maximum number of entries (from config).
  size_t capacity() const;

 private:
  struct CacheEntry {
    TrajectoryDTO trajectory;
    TimePoint stored_at;
    std::list<CacheKey>::iterator lru_iter;
  };

  void evict_lru();

  IClock& clock_;
  CacheConfig config_;
  mutable std::mutex mutex_;
  std::list<CacheKey> lru_order_;
  std::unordered_map<CacheKey, CacheEntry, CacheKeyHash> entries_;
};

}  // namespace manyforge::core
