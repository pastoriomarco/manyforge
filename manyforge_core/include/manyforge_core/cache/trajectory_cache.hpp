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

enum class TrajectoryCacheLookupDisposition {
  kHit,
  /// Same canonical intent, but validated against an older authoritative
  /// revision. The trajectory carries no authority until current validation
  /// succeeds and atomically restamps it.
  kRevalidationCandidate,
  kMiss,
  kAuthoritativeRevisionUnavailable,
  kRevisionMismatch,
  kIntentMismatch,
};

struct TrajectoryCacheLookupResult {
  TrajectoryCacheLookupDisposition disposition =
      TrajectoryCacheLookupDisposition::kMiss;
  std::optional<TrajectoryDTO> trajectory;
};

/// Trajectory cache indexed by move_id and admitted only when both the scene
/// revision and full canonical planner-intent identity match (spec 230 §3.1).
/// One trajectory per move_id. No LRU, no TTL.
/// Thread-safe.
///
/// Planner-intent mismatches remove the stale entry atomically. A strictly
/// newer authoritative revision retains a same-intent entry as an immutable
/// revalidation candidate; rollback and same-revision/hash conflicts evict.
class TrajectoryCache {
 public:
  explicit TrajectoryCache(CacheConfig config = {});

  /// Store trajectory for a move_id with the collision-world revision it
  /// was validated against (overwrites existing). There is deliberately no
  /// revision-less overload: every stored trajectory must declare what
  /// collision world it was validated against.
  void store(const std::string& move_id, TrajectoryDTO trajectory,
             CollisionWorldRevision revision,
             std::string intent_identity);

  /// Whether an entry exists for move_id, regardless of revision. Existence
  /// probe only (e.g. invalidate-if-present); never use to serve a hit.
  bool contains(const std::string& move_id) const;

  /// Revision-UNCHECKED read for observability/debug surfaces only. The
  /// returned trajectory may be stale relative to the live collision world;
  /// planning/execution callers must use lookup_with_revision.
  std::optional<TrajectoryDTO> peek_unvalidated(const std::string& move_id) const;

  /// Look up trajectory by move_id. Exact revision+intent returns kHit;
  /// same-intent older revision returns kRevalidationCandidate; intent mismatch
  /// is removed while holding the cache mutex.
  ///
  /// An expected revision with both `revision_id == 0` and an empty
  /// `content_hash` is treated as "no authoritative revision available";
  /// callers in that state MUST NOT serve a cache hit — this method
  /// returns kAuthoritativeRevisionUnavailable regardless of the stored entry.
  TrajectoryCacheLookupResult lookup_with_revision(
      const std::string& move_id,
      const CollisionWorldRevision& expected_revision,
      const std::string& expected_intent_identity);

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
    /// Canonical identity of the resolved motion, frames, tolerances, speed
    /// profile, and request-level dynamics limits that produced this trajectory.
    std::string intent_identity;
  };

  CacheConfig config_;
  mutable std::mutex mutex_;
  std::unordered_map<std::string, Entry> entries_;
  std::deque<std::string> insertion_order_;  // for FIFO eviction

  void erase_locked(const std::string& move_id);
};

}  // namespace manyforge::core
