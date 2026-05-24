#pragma once

#include <memory>
#include <optional>

#include <manyforge_core/types/collision.hpp>
#include <manyforge_core/types/collision_world_snapshot.hpp>

namespace manyforge::core {

/// Backend-agnostic publisher of `CollisionWorldSnapshot` (spec §3.8.1,
/// defined by the kernel-architecture sprint P-2-impl-snapshot chunk).
///
/// Publishers expose the scene-state half of the (world, policy) pair
/// that ICollisionQueryEngine workers consume. Each publisher has its
/// own monotonic revision domain (`publisher_id` in the snapshot)
/// and is responsible for producing deterministic snapshots from its
/// view of the collision world.
///
/// **Expected publishers:**
///   - RoboPlan-backed planning context (today, transitional).
///   - Isaac scene observer (Phase 2+).
///   - Deterministic fixture publisher (tests + qualification).
///   - Composer's editable scene store (long-term, once scene-edit
///     authority moves out of the planner).
///
/// **Snapshot reuse contract.**
///
/// Publishers SHOULD return the same `shared_ptr<const>` instance
/// across calls as long as the underlying world has not changed.
/// Engines compare `snapshot()->revision.revision_id` for cheap cache
/// hits; returning a fresh `shared_ptr` with an unchanged
/// `revision_id` is permitted but wasteful.
///
/// On any world mutation, publishers MUST advance `revision_id` by
/// at least 1 and re-run the snapshot builder so the `content_hash`
/// reflects the new state. Publishers MUST NOT mutate an
/// already-returned snapshot.
///
/// **Threading.**
///
/// `snapshot()` MUST be safe to call from any thread. The returned
/// snapshot is immutable; consumers share it across worker threads
/// without further synchronization. Implementations that build the
/// snapshot lazily on the first call MUST guard the build with their
/// own mutex.
///
/// **World / policy consistency.**
///
/// When a publisher also provides `collision_policy_snapshot()`, the
/// world snapshot and policy snapshot exposed to a validation pass MUST
/// come from one coherent internal commit. Implementations may satisfy
/// that requirement with a shared mutex, an atomic paired publication,
/// or an internal cached pair. Publishers that cannot make this guarantee
/// MUST return `std::nullopt` for `collision_policy_snapshot()` so the
/// kernel can fail closed or use an explicitly installed static policy.
class ICollisionWorldSnapshotPublisher {
 public:
  virtual ~ICollisionWorldSnapshotPublisher() = default;

  /// Return the current immutable snapshot. Returning `nullptr` is
  /// reserved for the "no scene available" diagnostic case and MUST
  /// cause the kernel to fail closed with
  /// `CollisionDecisionAction::kInconclusive`.
  [[nodiscard]] virtual CollisionWorldSnapshotPtr snapshot() const = 0;

  /// Return the current immutable collision policy snapshot that belongs with
  /// the current world snapshot. Implementations that publish only geometry may
  /// return nullopt; the kernel will then use an explicitly installed fallback
  /// policy snapshot if one exists, otherwise fail closed.
  ///
  /// When provided, this policy has priority over any static kernel-installed
  /// policy because scene edits such as attach/detach, touch-link changes, and
  /// support-contact discovery can change the effective policy at the same
  /// cadence as the collision world.
  [[nodiscard]] virtual std::optional<CollisionPolicySnapshot>
  collision_policy_snapshot() const {
    return std::nullopt;
  }
};

}  // namespace manyforge::core
