#include <manyforge_core/cache/trajectory_cache.hpp>

namespace manyforge::core {

TrajectoryCache::TrajectoryCache(IClock& clock, CacheConfig config)
    : clock_(clock), config_(config) {}

Result<TrajectoryDTO> TrajectoryCache::lookup(const CacheKey& key) {
  std::lock_guard<std::mutex> lock(mutex_);

  auto it = entries_.find(key);
  if (it == entries_.end()) {
    return Result<TrajectoryDTO>::Err(ErrorCode::kCacheMiss);
  }

  // Check TTL
  if (config_.ttl_ns > 0) {
    auto age = clock_.now() - it->second.stored_at;
    if (age > config_.ttl_ns) {
      lru_order_.erase(it->second.lru_iter);
      entries_.erase(it);
      return Result<TrajectoryDTO>::Err(ErrorCode::kCacheMiss);
    }
  }

  // Move to front of LRU
  lru_order_.erase(it->second.lru_iter);
  lru_order_.push_front(key);
  it->second.lru_iter = lru_order_.begin();

  return Result<TrajectoryDTO>::Ok(it->second.trajectory);
}

void TrajectoryCache::store(const CacheKey& key, TrajectoryDTO trajectory) {
  std::lock_guard<std::mutex> lock(mutex_);

  auto it = entries_.find(key);
  if (it != entries_.end()) {
    // Update existing entry
    it->second.trajectory = std::move(trajectory);
    it->second.stored_at = clock_.now();
    lru_order_.erase(it->second.lru_iter);
    lru_order_.push_front(key);
    it->second.lru_iter = lru_order_.begin();
    return;
  }

  // Evict if at capacity
  while (entries_.size() >= config_.max_entries && !entries_.empty()) {
    evict_lru();
  }

  lru_order_.push_front(key);
  entries_.insert(
      {key, CacheEntry{std::move(trajectory), clock_.now(), lru_order_.begin()}});
}

void TrajectoryCache::invalidate_for_scene(uint64_t current_scene_version) {
  std::lock_guard<std::mutex> lock(mutex_);

  auto it = entries_.begin();
  while (it != entries_.end()) {
    if (it->first.scene_version != current_scene_version) {
      lru_order_.erase(it->second.lru_iter);
      it = entries_.erase(it);
    } else {
      ++it;
    }
  }
}

void TrajectoryCache::invalidate_all() {
  std::lock_guard<std::mutex> lock(mutex_);
  lru_order_.clear();
  entries_.clear();
}

size_t TrajectoryCache::size() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return entries_.size();
}

size_t TrajectoryCache::capacity() const {
  return config_.max_entries;
}

void TrajectoryCache::evict_lru() {
  if (lru_order_.empty()) return;
  auto oldest_key = lru_order_.back();
  lru_order_.pop_back();
  entries_.erase(oldest_key);
}

}  // namespace manyforge::core
