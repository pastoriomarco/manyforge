#include <manyforge_core/cache/trajectory_cache.hpp>

#include <algorithm>

namespace manyforge::core {

TrajectoryCache::TrajectoryCache(CacheConfig config)
    : config_(config) {}

void TrajectoryCache::store(const std::string& move_id, TrajectoryDTO trajectory) {
  std::lock_guard<std::mutex> lock(mutex_);

  auto it = entries_.find(move_id);
  if (it != entries_.end()) {
    it->second = std::move(trajectory);
    return;
  }

  // Evict oldest if at capacity (FIFO).
  while (entries_.size() >= config_.max_entries && !insertion_order_.empty()) {
    entries_.erase(insertion_order_.front());
    insertion_order_.pop_front();
  }

  entries_.emplace(move_id, std::move(trajectory));
  insertion_order_.push_back(move_id);
}

std::optional<TrajectoryDTO> TrajectoryCache::lookup(const std::string& move_id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = entries_.find(move_id);
  if (it == entries_.end()) {
    return std::nullopt;
  }
  return it->second;
}

void TrajectoryCache::invalidate(const std::string& move_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (entries_.erase(move_id) > 0) {
    auto it = std::find(insertion_order_.begin(), insertion_order_.end(), move_id);
    if (it != insertion_order_.end()) {
      insertion_order_.erase(it);
    }
  }
}

void TrajectoryCache::invalidate_all() {
  std::lock_guard<std::mutex> lock(mutex_);
  entries_.clear();
  insertion_order_.clear();
}

size_t TrajectoryCache::size() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return entries_.size();
}

size_t TrajectoryCache::capacity() const {
  return config_.max_entries;
}

}  // namespace manyforge::core
