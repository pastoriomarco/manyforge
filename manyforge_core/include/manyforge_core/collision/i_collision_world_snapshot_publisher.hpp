#pragma once

#include <cstddef>
#include <cstdint>
#include <manyforge_core/types/collision.hpp>
#include <manyforge_core/types/collision_world_snapshot.hpp>
#include <manyforge_core/types/logical_scene_snapshot.hpp>
#include <manyforge_core/types/projection_common.hpp>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace manyforge::core {

struct CollisionPolicyIndex;

struct CollisionWorldPolicySnapshotBundle {
  CollisionWorldPolicySnapshotBundle() = default;
  CollisionWorldPolicySnapshotBundle(
      CollisionWorldSnapshotPtr world_snapshot,
      std::optional<CollisionPolicySnapshot> policy_snapshot = std::nullopt)
      : world(std::move(world_snapshot)), policy(std::move(policy_snapshot)) {}

  CollisionWorldSnapshotPtr world;
  std::optional<CollisionPolicySnapshot> policy;
  /// Operation-local clearance margins installed by the Kernel at admission
  /// from `world`, `policy` and the request. Publishers leave it null.
  std::shared_ptr<const CollisionClearanceMargins> clearance_margins;
};

/// One immutable, coherent publication consumed by runtime-safety ticks.
/// Writers build policy adaptation and indexes before publication, then swap a
/// single shared handle. Consumers read the latest handle; a different handle
/// is a newer publication.
struct RuntimeCollisionQueryBundle {
  /// Retained-bundle/graph/logical-scene projection that produced this
  /// publication. Legacy/test-only publishers may omit it.
  std::optional<ProjectionRevisionDTO> projection_revision;
  CollisionWorldSnapshotPtr world;
  std::shared_ptr<const CollisionPolicySnapshot> policy;
  std::shared_ptr<const CollisionPolicyIndex> policy_index;
  /// Pre-resolved metric selections prepared on the NRT publication path.
  /// The default vector contains signed-release and bounded-penetration pairs.
  /// Each contact-grasp entry contains that same base union plus the concrete
  /// world body pairs matched by the active grasp binding. Binary release
  /// pairs are intentionally absent.
  std::vector<CollisionPairId> selected_distance_pairs;
  std::map<std::string, std::vector<CollisionPairId>, std::less<>>
      contact_grasp_selected_distance_pairs;
  bool distance_selection_order_valid = false;
  /// Publication-time proof that world bodies are sorted and unique by body
  /// id, enabling allocation-free lookup per tick.
  bool lookup_order_valid = false;
  /// Set only on a runtime-query session's own bundle: the latest published
  /// world paired with the move's admitted operation-local policy. Never set by
  /// a publisher.
  bool admitted_operation_policy = false;
};

using RuntimeCollisionQueryBundlePtr =
    std::shared_ptr<const RuntimeCollisionQueryBundle>;

struct CollisionWorldLeaseInventory {
  ProjectionLeaseBindingDTO projection_binding;
  /// Exact immutable logical snapshot retained with the native/collision
  /// projection. Reference resolution must consume this value rather than a
  /// later mutable scene-owner read.
  std::shared_ptr<const LogicalSceneSnapshotDTO> logical_scene;
  std::int64_t captured_at_ns = 0;
  CollisionWorldPolicySnapshotBundle bundle;
};

struct CollisionWorldLeaseRetentionStats {
  std::size_t retained_count = 0;
  std::size_t retained_capacity = 0;
  std::int64_t retained_max_age_ns = 0;
  std::vector<ProjectionLeaseId> retained_ids_oldest_first;
};

/// Backend-agnostic publisher of `CollisionWorldSnapshot`.
///
/// Publishers expose the scene-state half of the (world, policy) pair
/// that ICollisionQueryEngine workers consume. Each publisher has its
/// own monotonic revision domain (`publisher_id` in the snapshot)
/// and is responsible for producing deterministic snapshots from its
/// view of the collision world.
///
/// **Expected publishers:**
///   - RoboPlan-backed observed-state provider (today, transitional).
///   - Isaac scene observer (planned).
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
  /// to borrow or rebuild the full snapshot. Admission and diagnostics use this
  /// as a cheap revision probe; runtime ticks consume the coherent immutable
  /// query bundle below instead. Implementations should override this with a
  /// leaf/atomic revision read; the default is correct but may be more
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

  /// Return the latest logical scene with its world+policy bundle, as one
  /// coherent value. Unlike the runtime monitor surface below, this is an
  /// admission/planning surface and may borrow the scene lock. Planning reads
  /// it at plan time; validation judges freshness on it. Implementations that
  /// cannot provide it return nullopt.
  [[nodiscard]] virtual std::optional<CollisionWorldLeaseInventory>
  current_scene_snapshot_lease_inventory() const {
    return std::nullopt;
  }

  /// Retain the current inventory under one lease id so planning keeps using
  /// the immutable world+policy bundle it started with while newer scenes are
  /// published. The returned shared pointers remain valid even after release;
  /// release only removes the publisher's lookup entry, if it keeps one.
  [[nodiscard]] virtual std::optional<CollisionWorldLeaseInventory>
  retain_scene_snapshot_lease_inventory(ProjectionLeaseId projection_lease_id =
                                            ProjectionLeaseId{std::string{}}) {
    (void)projection_lease_id;
    return std::nullopt;
  }

  /// Release one retained lease inventory entry. Missing ids are a no-op:
  /// callers may race cancellation/timeout cleanup without turning cleanup into
  /// a new failure source.
  virtual void release_scene_snapshot_lease_inventory(
      const ProjectionLeaseId &projection_lease_id) {
    (void)projection_lease_id;
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
  [[nodiscard]] virtual RuntimeCollisionQueryBundlePtr
  published_runtime_collision_query_bundle() const = 0;
};

} // namespace manyforge::core
