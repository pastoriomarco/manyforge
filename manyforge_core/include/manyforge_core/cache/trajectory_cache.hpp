#pragma once

#include <deque>
#include <manyforge_core/cache/cache_policy.hpp>
#include <manyforge_core/types/collision.hpp>
#include <manyforge_core/types/projection_common.hpp>
#include <manyforge_core/types/strong_id.hpp>
#include <manyforge_core/types/trajectory_dto.hpp>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

namespace manyforge::core {

/// Stable program-scoped cache slot from spec 210 section 3.10.
///
/// A source-authored `move_id` is not globally unique: repeated Program runs,
/// different bindings, targets, and capabilities may legitimately reuse it.
/// Keeping the complete slot identity in the cache prevents those independent
/// operations from evicting or serving one another's trajectories.
struct TrajectoryCacheKey final {
  ProgramRevision program_revision{std::string{}};
  ProgramBindingId program_binding_id{std::string{}};
  ProgramRunId program_run_id{std::string{}};
  MoveId move_id{std::string{}};
  ControllableComponentId controllable_component_id{std::string{}};
  CommandTargetId command_target_id{std::string{}};
  CapabilityId capability_id{std::string{}};

  [[nodiscard]] bool valid() const noexcept {
    return !program_revision.empty() && !program_binding_id.empty() && !program_run_id.empty() &&
           !move_id.empty() && !controllable_component_id.empty() && !command_target_id.empty() &&
           !capability_id.empty();
  }

  friend bool operator==(const TrajectoryCacheKey&, const TrajectoryCacheKey&) = default;
};

/// Exact immutable evidence against which a trajectory was admitted.
/// `ProjectionRevisionDTO` prevents a cache entry from silently crossing a
/// retained bundle, graph, logical-scene lineage, or scene revision. The
/// collision-world revision additionally identifies the concrete normalized
/// geometry/policy publication used by authoritative validation.
struct TrajectoryCacheRevisionEvidence final {
  ProjectionRevisionDTO projection_revision;
  CollisionWorldRevision collision_world_revision;

  [[nodiscard]] bool valid() const noexcept {
    return projection_revision.valid() && collision_world_revision.revision_id != 0U &&
           !collision_world_revision.content_hash.empty();
  }

  friend bool operator==(const TrajectoryCacheRevisionEvidence& left,
                         const TrajectoryCacheRevisionEvidence& right) {
    return left.projection_revision == right.projection_revision &&
           left.collision_world_revision.revision_id ==
               right.collision_world_revision.revision_id &&
           left.collision_world_revision.content_hash ==
               right.collision_world_revision.content_hash;
  }
};

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
  TrajectoryCacheLookupDisposition disposition = TrajectoryCacheLookupDisposition::kMiss;
  std::optional<TrajectoryDTO> trajectory;
};

/// Trajectory cache indexed by the complete stable Program/move slot and
/// admitted only when both the exact projection evidence and full canonical
/// planner-intent identity match (spec 210 §3.10 and spec 230 §3.1).
/// One trajectory per exact slot. No LRU, no TTL.
/// Thread-safe.
///
/// Planner-intent mismatches remove the stale entry atomically. A strictly
/// newer authoritative revision retains a same-intent entry as an immutable
/// revalidation candidate; rollback and same-revision/hash conflicts evict.
class TrajectoryCache {
 public:
  explicit TrajectoryCache(CacheConfig config = {});

  /// Store a trajectory with the complete immutable projection evidence it
  /// was validated against (overwrites the same exact slot). There is
  /// deliberately no evidence-less overload.
  void store(const TrajectoryCacheKey& key, TrajectoryDTO trajectory,
             TrajectoryCacheRevisionEvidence evidence, std::string intent_identity);

  /// Whether an entry exists for an exact slot, regardless of revision. Existence
  /// probe only (e.g. invalidate-if-present); never use to serve a hit.
  bool contains(const TrajectoryCacheKey& key) const;

  /// Revision-UNCHECKED read for observability/debug surfaces only. The
  /// returned trajectory may be stale relative to the live collision world;
  /// planning/execution callers must use lookup_with_revision.
  std::optional<TrajectoryDTO> peek_unvalidated(const TrajectoryCacheKey& key) const;

  /// Look up trajectory by move_id. Exact revision+intent returns kHit;
  /// same-intent older revision returns kRevalidationCandidate; intent mismatch
  /// is removed while holding the cache mutex.
  ///
  /// An expected revision with both `revision_id == 0` and an empty
  /// `content_hash` is treated as "no authoritative revision available";
  /// callers in that state MUST NOT serve a cache hit — this method
  /// returns kAuthoritativeRevisionUnavailable regardless of the stored entry.
  TrajectoryCacheLookupResult lookup_with_revision(
      const TrajectoryCacheKey& key, const TrajectoryCacheRevisionEvidence& expected_evidence,
      const std::string& expected_intent_identity);

  /// Stored revision for an entry, when present. Returns std::nullopt if
  /// the entry is missing or was stored without a revision tag.
  std::optional<TrajectoryCacheRevisionEvidence> revision_of(const TrajectoryCacheKey& key) const;

  /// Invalidate one exact slot.
  void invalidate(const TrajectoryCacheKey& key);

  /// Remove all entries.
  void invalidate_all();

  /// Number of cached entries.
  size_t size() const;

  /// Max entries (safety bound).
  size_t capacity() const;

 private:
  struct Entry {
    TrajectoryCacheKey key;
    /// Immutable shared trajectory: lookups grab the pointer under the
    /// mutex and copy the payload after releasing it, so the lock hold
    /// time is O(1) instead of O(waypoints).
    std::shared_ptr<const TrajectoryDTO> trajectory;
    /// Projection evidence the trajectory was validated against.
    TrajectoryCacheRevisionEvidence evidence;
    /// Canonical identity of the resolved motion, frames, tolerances, speed
    /// profile, and request-level dynamics limits that produced this trajectory.
    std::string intent_identity;
  };

  CacheConfig config_;
  mutable std::mutex mutex_;
  std::unordered_map<std::string, Entry> entries_;
  std::deque<std::string> insertion_order_;  // for FIFO eviction

  [[nodiscard]] static std::string encoded_key(const TrajectoryCacheKey& key);
  void erase_locked(const std::string& encoded);
};

}  // namespace manyforge::core
