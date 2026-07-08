#pragma once

#include <memory>
#include <optional>

#include <manyforge_core/types/collision.hpp>
#include <manyforge_core/types/collision_world_snapshot.hpp>

namespace manyforge::core {

struct CollisionWorldPolicySnapshotBundle {
  CollisionWorldSnapshotPtr world;
  std::optional<CollisionPolicySnapshot> policy;
};

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

  /// Return the publisher's current world revision without requiring consumers
  /// to borrow or rebuild the full snapshot. Runtime monitors use this as a
  /// cheap coherency probe: if the revision differs from the currently
  /// published runtime snapshot, they refresh the full world+policy bundle
  /// before checking live robot state. Implementations should override this
  /// with a leaf/atomic revision read; the default is correct but may be more
  /// expensive because it borrows `snapshot()`.
  [[nodiscard]] virtual CollisionWorldRevision
  published_collision_world_revision() const {
    auto current = snapshot();
    return current ? current->revision : CollisionWorldRevision{};
  }

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

  /// Return the coherent world+policy pair for runtime monitor and
  /// authoritative validation consumers. Implementations with a shared scene
  /// mutex should override this and publish both values under one lock.
  [[nodiscard]] virtual CollisionWorldPolicySnapshotBundle
  collision_world_policy_snapshot_bundle() const {
    return CollisionWorldPolicySnapshotBundle{snapshot(),
                                              collision_policy_snapshot()};
  }

  /// Return the pre-published runtime-monitor world+policy bundle.
  ///
  /// This is the RT path. Implementations MUST serve this from an immutable
  /// publish-on-write cache, atomic pointer, seqlock, or other leaf data
  /// structure. It must not borrow planner/scene locks, rebuild collision
  /// geometry, or recompute policy. Writers that mutate the collision world or
  /// effective collision policy are responsible for publishing a coherent
  /// bundle before exposing the new revision to runtime monitors.
  [[nodiscard]] virtual CollisionWorldPolicySnapshotBundle
  published_runtime_collision_snapshot_bundle() const = 0;
};

}  // namespace manyforge::core
