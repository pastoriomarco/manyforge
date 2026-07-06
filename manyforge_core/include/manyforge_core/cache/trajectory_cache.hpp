#pragma once

#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

#include <manyforge_core/cache/cache_policy.hpp>
#include <manyforge_core/types/collision.hpp>
#include <manyforge_core/types/trajectory_dto.hpp>

namespace manyforge::core {

/// Trajectory cache keyed by move_id (spec 230 §3.1).
/// One trajectory per move_id. No LRU, no TTL.
/// Thread-safe.
///
/// Chunk 2 of the collision-ownership plan adds revision-aware entries:
/// every stored trajectory MAY carry the CollisionWorldRevision it was
/// validated against. Revision-aware lookups (lookup_with_revision) treat
/// a revision mismatch as a miss and remove the stale entry so the next
/// caller sees no ambiguity. The legacy revision-less overloads remain for
/// non-revision-aware code paths (kernel call sites that have not yet
/// migrated, and unit tests).
class TrajectoryCache {
 public:
  explicit TrajectoryCache(CacheConfig config = {});

  /// Store trajectory for a move_id with the collision-world revision it
  /// was validated against (overwrites existing). There is deliberately no
  /// revision-less overload: every stored trajectory must declare what
  /// collision world it was validated against.
  void store(const std::string& move_id, TrajectoryDTO trajectory,
             CollisionWorldRevision revision);

  /// Whether an entry exists for move_id, regardless of revision. Existence
  /// probe only (e.g. invalidate-if-present); never use to serve a hit.
  bool contains(const std::string& move_id) const;

  /// Revision-UNCHECKED read for observability/debug surfaces only. The
  /// returned trajectory may be stale relative to the live collision world;
  /// planning/execution callers must use lookup_with_revision.
  std::optional<TrajectoryDTO> peek_unvalidated(const std::string& move_id) const;

  /// Look up trajectory by move_id, returning the trajectory only when its
  /// stored revision matches `expected_revision`. On a revision mismatch,
  /// the stale entry is removed and std::nullopt is returned so subsequent
  /// lookups see no ambiguity.
  ///
  /// An expected revision with both `revision_id == 0` and an empty
  /// `content_hash` is treated as "no authoritative revision available";
  /// callers in that state MUST NOT serve a cache hit — this method
  /// returns std::nullopt regardless of the stored entry.
  std::optional<TrajectoryDTO> lookup_with_revision(
      const std::string& move_id,
      const CollisionWorldRevision& expected_revision);

  /// Stored revision for an entry, when present. Returns std::nullopt if
  /// the entry is missing or was stored without a revision tag.
  std::optional<CollisionWorldRevision> revision_of(
      const std::string& move_id) const;

  /// Invalidate a specific move_id entry.
  void invalidate(const std::string& move_id);

  /// Remove all entries.
  void invalidate_all();

  /// Number of cached entries.
  size_t size() const;

  /// Max entries (safety bound).
  size_t capacity() const;

 private:
  struct Entry {
    /// Immutable shared trajectory: lookups grab the pointer under the
    /// mutex and copy the payload after releasing it, so the lock hold
    /// time is O(1) instead of O(waypoints).
    std::shared_ptr<const TrajectoryDTO> trajectory;
    /// Revision the trajectory was validated against.
    CollisionWorldRevision revision;
  };

  CacheConfig config_;
  mutable std::mutex mutex_;
  std::unordered_map<std::string, Entry> entries_;
  std::deque<std::string> insertion_order_;  // for FIFO eviction

  void erase_locked(const std::string& move_id);
};

}  // namespace manyforge::core
