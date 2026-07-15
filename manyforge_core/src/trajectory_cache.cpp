#include <manyforge_core/cache/trajectory_cache.hpp>

#include <algorithm>

namespace manyforge::core {

TrajectoryCache::TrajectoryCache(CacheConfig config)
    : config_(config) {}

void TrajectoryCache::store(const std::string& move_id, TrajectoryDTO trajectory,
                            CollisionWorldRevision revision,
                            std::string intent_identity) {
  // Build the shared payload outside the lock; the critical section only
  // swaps pointers and maintains the FIFO index.
  auto shared_trajectory =
      std::make_shared<const TrajectoryDTO>(std::move(trajectory));

  std::lock_guard<std::mutex> lock(mutex_);

  auto it = entries_.find(move_id);
  if (it != entries_.end()) {
    it->second.trajectory = std::move(shared_trajectory);
    it->second.revision = std::move(revision);
    it->second.intent_identity = std::move(intent_identity);
    return;
  }

  // Evict oldest if at capacity (FIFO).
  while (entries_.size() >= config_.max_entries && !insertion_order_.empty()) {
    entries_.erase(insertion_order_.front());
    insertion_order_.pop_front();
  }

  entries_.emplace(move_id,
                   Entry{std::move(shared_trajectory), std::move(revision),
                         std::move(intent_identity)});
  insertion_order_.push_back(move_id);
}

bool TrajectoryCache::contains(const std::string& move_id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  return entries_.find(move_id) != entries_.end();
}

std::optional<TrajectoryDTO> TrajectoryCache::peek_unvalidated(
    const std::string& move_id) const {
  std::shared_ptr<const TrajectoryDTO> found;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = entries_.find(move_id);
    if (it == entries_.end()) {
      return std::nullopt;
    }
    found = it->second.trajectory;
  }
  // Deep copy outside the lock.
  return *found;
}

TrajectoryCacheLookupResult TrajectoryCache::lookup_with_revision(
    const std::string& move_id,
    const CollisionWorldRevision& expected_revision,
    const std::string& expected_intent_identity) {
  const bool expected_is_unset = expected_revision.revision_id == 0 &&
                                 expected_revision.content_hash.empty();

  std::unique_lock<std::mutex> lock(mutex_);
  auto it = entries_.find(move_id);
  if (it == entries_.end()) {
    return {TrajectoryCacheLookupDisposition::kMiss, std::nullopt};
  }

  const auto& stored = it->second.revision;
  const bool stored_is_unset =
      stored.revision_id == 0 && stored.content_hash.empty();

  // Trust-the-trust-boundary policy:
  // - Expected unset: caller has no authoritative collision-world revision.
  //   Do not serve a revision-aware hit.
  // - Expected unset, stored tagged: caller did not provide an
  //   authoritative revision; do not serve a tagged entry. Treat as miss
  //   without evicting (the entry may be valid for a future caller that
  //   does supply a revision).
  // - Expected tagged, stored unset: adapter started publishing revisions
  //   after this entry was stored. The stored entry is no longer trusted;
  //   evict and miss so the kernel revalidates.
  // - Both tagged: exact match is a hit. A same-intent entry is a
  //   revalidation candidate only when the authoritative revision advanced.
  //   Revision rollback or a different hash under the same revision is an
  //   integrity failure, not an "older snapshot", and is evicted fail-closed.
  if (expected_is_unset) {
    return {TrajectoryCacheLookupDisposition::kAuthoritativeRevisionUnavailable,
            std::nullopt};
  }
  if (!expected_is_unset && stored_is_unset) {
    erase_locked(move_id);
    return {TrajectoryCacheLookupDisposition::kRevisionMismatch,
            std::nullopt};
  }
  if (it->second.intent_identity != expected_intent_identity) {
    erase_locked(move_id);
    return {TrajectoryCacheLookupDisposition::kIntentMismatch, std::nullopt};
  }
  const bool revision_matches =
      stored.revision_id == expected_revision.revision_id &&
      stored.content_hash == expected_revision.content_hash;
  const bool authoritative_revision_advanced =
      expected_revision.revision_id > stored.revision_id;
  if (!revision_matches && !authoritative_revision_advanced) {
    erase_locked(move_id);
    return {TrajectoryCacheLookupDisposition::kRevisionMismatch,
            std::nullopt};
  }
  // Hold the shared payload, then deep-copy outside the lock so the hit
  // path's lock hold time stays O(1).
  std::shared_ptr<const TrajectoryDTO> found = it->second.trajectory;
  lock.unlock();
  return {revision_matches
              ? TrajectoryCacheLookupDisposition::kHit
              : TrajectoryCacheLookupDisposition::kRevalidationCandidate,
          *found};
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
