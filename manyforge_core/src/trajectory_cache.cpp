#include <manyforge_core/cache/trajectory_cache.hpp>

#include <algorithm>

namespace manyforge::core {

TrajectoryCache::TrajectoryCache(CacheConfig config)
    : config_(config) {}

void TrajectoryCache::store(const std::string& move_id, TrajectoryDTO trajectory,
                            CollisionWorldRevision revision) {
  std::lock_guard<std::mutex> lock(mutex_);

  auto it = entries_.find(move_id);
  if (it != entries_.end()) {
    it->second.trajectory = std::move(trajectory);
    it->second.revision = std::move(revision);
    return;
  }

  // Evict oldest if at capacity (FIFO).
  while (entries_.size() >= config_.max_entries && !insertion_order_.empty()) {
    entries_.erase(insertion_order_.front());
    insertion_order_.pop_front();
  }

  entries_.emplace(move_id, Entry{std::move(trajectory), std::move(revision)});
  insertion_order_.push_back(move_id);
}

bool TrajectoryCache::contains(const std::string& move_id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  return entries_.find(move_id) != entries_.end();
}

std::optional<TrajectoryDTO> TrajectoryCache::peek_unvalidated(
    const std::string& move_id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = entries_.find(move_id);
  if (it == entries_.end()) {
    return std::nullopt;
  }
  return it->second.trajectory;
}

std::optional<TrajectoryDTO> TrajectoryCache::lookup_with_revision(
    const std::string& move_id,
    const CollisionWorldRevision& expected_revision) {
  const bool expected_is_unset = expected_revision.revision_id == 0 &&
                                 expected_revision.content_hash.empty();

  std::lock_guard<std::mutex> lock(mutex_);
  auto it = entries_.find(move_id);
  if (it == entries_.end()) {
    return std::nullopt;
  }

  const auto& stored = it->second.revision;
  const bool stored_is_unset =
      stored.revision_id == 0 && stored.content_hash.empty();

  // Trust-the-trust-boundary policy:
  // - Expected unset: caller has no authoritative collision-world revision.
  //   Do not serve a revision-aware hit. The legacy lookup() overload remains
  //   available for explicitly legacy call sites.
  // - Expected unset, stored tagged: caller did not provide an
  //   authoritative revision; do not serve a tagged entry. Treat as miss
  //   without evicting (the entry may be valid for a future caller that
  //   does supply a revision).
  // - Expected tagged, stored unset: adapter started publishing revisions
  //   after this entry was stored. The stored entry is no longer trusted;
  //   evict and miss so the kernel revalidates.
  // - Both tagged: must match exactly. Mismatch evicts the stale entry.
  if (expected_is_unset) {
    return std::nullopt;
  }
  if (!expected_is_unset && stored_is_unset) {
    erase_locked(move_id);
    return std::nullopt;
  }
  if (stored.revision_id != expected_revision.revision_id ||
      stored.content_hash != expected_revision.content_hash) {
    erase_locked(move_id);
    return std::nullopt;
  }
  return it->second.trajectory;
}

std::optional<CollisionWorldRevision> TrajectoryCache::revision_of(
    const std::string& move_id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = entries_.find(move_id);
  if (it == entries_.end()) {
    return std::nullopt;
  }
  const auto& stored = it->second.revision;
  if (stored.revision_id == 0 && stored.content_hash.empty()) {
    return std::nullopt;
  }
  return stored;
}

void TrajectoryCache::invalidate(const std::string& move_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  erase_locked(move_id);
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

void TrajectoryCache::erase_locked(const std::string& move_id) {
  if (entries_.erase(move_id) > 0) {
    auto it = std::find(insertion_order_.begin(), insertion_order_.end(), move_id);
    if (it != insertion_order_.end()) {
      insertion_order_.erase(it);
    }
  }
}

}  // namespace manyforge::core
