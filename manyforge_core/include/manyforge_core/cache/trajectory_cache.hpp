#pragma once

#include <cstdint>
#include <deque>
#include <manyforge_core/cache/cache_policy.hpp>
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

enum class TrajectoryCacheLookupDisposition {
  kFound,
  kMiss,
  kIntentMismatch,
};

struct TrajectoryCacheLookupResult {
  TrajectoryCacheLookupDisposition disposition = TrajectoryCacheLookupDisposition::kMiss;
  std::optional<TrajectoryDTO> trajectory;
  /// Planning attempt that produced the trajectory; a reuse is revalidated at
  /// that attempt's collision-check step.
  std::uint32_t planning_attempt_index = 0;
};

/// Trajectory cache indexed by the complete stable Program/move slot and the
/// full canonical planner-intent identity (spec 210 §3.10 and spec 230 §3.1).
/// One trajectory per exact slot. No LRU, no TTL.
/// Thread-safe.
///
/// A stored trajectory carries no authority: the caller validates it against
/// the latest scene and limits before use, exactly like a fresh plan, and
/// re-plans when that fails. A planner-intent mismatch removes the entry
/// atomically. Robot or model changes are covered by clearing the whole cache
/// at every Apply.
class TrajectoryCache {
 public:
  explicit TrajectoryCache(CacheConfig config = {});

  /// Store a trajectory with the planner intent and the planning attempt that
  /// produced it (overwrites the same exact slot).
  void store(const TrajectoryCacheKey& key, TrajectoryDTO trajectory, std::string intent_identity,
             std::uint32_t planning_attempt_index = 0);

  /// Whether an entry exists for an exact slot. Existence probe only (e.g.
  /// invalidate-if-present); never use to serve a trajectory.
  bool contains(const TrajectoryCacheKey& key) const;

  /// Unvalidated read for observability/debug surfaces only. Planning and
  /// execution callers must use lookup and validate the result.
  std::optional<TrajectoryDTO> peek_unvalidated(const TrajectoryCacheKey& key) const;

  /// Stored trajectory for an exact slot and intent. An intent mismatch is
  /// removed while holding the cache mutex.
  TrajectoryCacheLookupResult lookup(const TrajectoryCacheKey& key,
                                     const std::string& expected_intent_identity);

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
    /// Canonical identity of the resolved motion, frames, tolerances, speed
    /// profile, and request-level dynamics limits that produced this trajectory.
    std::string intent_identity;
    std::uint32_t planning_attempt_index = 0;
  };

  CacheConfig config_;
  mutable std::mutex mutex_;
  std::unordered_map<std::string, Entry> entries_;
  std::deque<std::string> insertion_order_;  // for FIFO eviction

  [[nodiscard]] static std::string encoded_key(const TrajectoryCacheKey& key);
  void erase_locked(const std::string& encoded);
};

}  // namespace manyforge::core
