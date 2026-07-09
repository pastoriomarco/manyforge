#pragma once

#include <cstdint>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <manyforge_core/collision/scene_snapshot_lease.hpp>
#include <manyforge_core/types/collision.hpp>
#include <manyforge_core/types/collision_world_snapshot.hpp>

namespace manyforge::core {

struct CollisionWorldBodyFreshness {
  std::string body_id;
  std::optional<std::string> source_id;
  std::optional<std::int64_t> source_timestamp_ns;
  std::optional<std::int64_t> source_freshness_budget_ns;
};

struct CollisionWorldPolicySnapshotBundle {
  CollisionWorldPolicySnapshotBundle() = default;
  CollisionWorldPolicySnapshotBundle(
      CollisionWorldSnapshotPtr world_snapshot,
      std::optional<CollisionPolicySnapshot> policy_snapshot = std::nullopt,
      std::vector<CollisionWorldBodyFreshness> freshness = {})
      : world(std::move(world_snapshot)),
        policy(std::move(policy_snapshot)),
        body_freshness(std::move(freshness)) {}

  CollisionWorldSnapshotPtr world;
  std::optional<CollisionPolicySnapshot> policy;

  /// Freshness sidecar for runtime monitor ticks.
  ///
  /// Geometry/topology/policy changes still require a new immutable world
  /// snapshot and revision. Stationary observed-source heartbeats update this
  /// sidecar only, allowing dynamic-body freshness checks to see a fresh source
  /// timestamp without rebuilding collision geometry or policy.
  ///
  /// Publishers MUST sort this vector by `body_id` so runtime ticks can perform
  /// allocation-free lookup while walking snapshot bodies.
  std::vector<CollisionWorldBodyFreshness> body_freshness;
};

struct CollisionWorldLeaseInventory {
  std::string retention_id;
  std::int64_t captured_at_ns = 0;
  CollisionWorldPolicySnapshotBundle bundle;
  std::uint64_t topology_epoch = 0;
  std::uint64_t policy_epoch = 0;
  std::vector<SceneSnapshotLiveObject> live_objects;
};

struct CollisionWorldLeaseRetentionStats {
  std::size_t retained_count = 0;
  std::size_t retained_capacity = 0;
  std::int64_t retained_max_age_ns = 0;
  std::vector<std::string> retained_ids_oldest_first;
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
    CollisionWorldPolicySnapshotBundle bundle;
    bundle.world = snapshot();
    bundle.policy = collision_policy_snapshot();
    return bundle;
  }

  /// Return the current world+policy+object-token inventory used to build an
  /// observed-scene snapshot lease. Unlike the runtime monitor surface below,
  /// this is an admission/planning surface and may borrow the scene lock to
  /// capture a coherent MVCC view. Implementations that cannot provide object
  /// tokens return nullopt so the kernel can fail closed or fall back to the
  /// strict revision-equality gate until RT-7D wires the new contract.
  [[nodiscard]] virtual std::optional<CollisionWorldLeaseInventory>
  current_scene_snapshot_lease_inventory() const {
    return std::nullopt;
  }

  /// Retain the current lease inventory so admission can keep using the exact
  /// immutable world+policy bundle while live scene revisions continue. The
  /// returned shared pointers remain valid even after release; release only
  /// removes this publisher's lookup/retention entry.
  [[nodiscard]] virtual std::optional<CollisionWorldLeaseInventory>
  retain_scene_snapshot_lease_inventory(std::string retention_id = {}) {
    (void)retention_id;
    return std::nullopt;
  }

  /// Release one retained lease inventory entry. Missing ids are a no-op:
  /// callers may race cancellation/timeout cleanup without turning cleanup into
  /// a new failure source.
  virtual void release_scene_snapshot_lease_inventory(
      const std::string& retention_id) {
    (void)retention_id;
  }

  [[nodiscard]] virtual CollisionWorldLeaseRetentionStats
  scene_snapshot_lease_retention_stats() const {
    return {};
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
