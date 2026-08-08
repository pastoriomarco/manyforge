#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <manyforge_core/types/collision.hpp>
#include <manyforge_core/types/geometry.hpp>
#include <manyforge_core/types/strong_id.hpp>

namespace manyforge::core {

/// Shared spelling authority for collision-body identities projected from the
/// retained model map and logical scene. Program Binding, scene projection,
/// and operation-local policy code must not reproduce these strings.
[[nodiscard]] inline CollisionBodyId make_component_collision_body_id(
    const ControllableComponentId& component_id,
    const ModelInstanceId& model_instance_id,
    const CollisionBodyRoleId& body_role_id, std::size_t collision_index) {
  return CollisionBodyId("component/" + component_id.value() + "/model/" +
                         model_instance_id.value() + "/body/" +
                         body_role_id.value() + "/collision/" +
                         std::to_string(collision_index));
}

[[nodiscard]] inline CollisionBodyId make_scene_collision_body_id(
    const SceneResourceId& scene_resource_id, std::size_t shape_index) {
  return CollisionBodyId("scene/" + scene_resource_id.value() + "/shape/" +
                         std::to_string(shape_index));
}

/// One collision body present in a `CollisionWorldSnapshot` (spec
/// §3.8.1, defined by the kernel-architecture sprint
/// P-2-impl-snapshot chunk).
///
/// A body is the unit of pose: every geometry attached to the body
/// inherits the body's `pose` and is offset by its own
/// `CollisionGeometryDescriptor::local_pose`. A single body may carry
/// multiple geometries (e.g. a component-model body with several primitives in
/// its `<collision>` URDF block).
///
/// Bodies are immutable for the lifetime of the snapshot they belong
/// to. Mutating an existing snapshot's body is a contract violation;
/// publishers MUST construct a fresh snapshot for any change and
/// advance the snapshot's `revision`.
struct CollisionWorldBody {
  /// Body identifier in the spec §3.8.1 normative format:
  /// `component/<component>/model/<model>/body/<role>/collision/<index>`,
  /// `scene/<scene_resource>/shape/<index>`, or
  /// `runtime_object/<runtime_object>/shape/<index>`.
  CollisionBodyId body_id;

  /// Portable owner category. Attached and detached scene resources retain
  /// the same owner identity; parent_body_id distinguishes a carried body.
  CollisionOwnerKind owner_kind = CollisionOwnerKind::kSceneResource;

  /// Exact component-model collision body carrying this scene resource.
  /// Required for a carried scene-resource body and absent otherwise.
  std::optional<CollisionBodyId> parent_body_id;

  /// Frame in which `pose` is expressed. Typically the collision-world frame
  /// for component-model, detached scene-resource, and runtime-object bodies;
  /// carried scene resources use their parent component-model body frame.
  std::string frame_id;

  /// Pose of the body within `frame_id` at snapshot time.
  Pose3DTO pose;

  /// Optional timestamp of the upstream observation that produced this body
  /// pose. Leave unset for static authored geometry. Dynamic pose-source /
  /// simulator-observed bodies SHOULD set this together with
  /// `source_freshness_budget_ns`; runtime monitors then fail closed when the
  /// body pose has aged beyond its declared budget.
  std::optional<std::string> source_id;
  std::optional<std::int64_t> source_timestamp_ns;

  /// Optional freshness budget for `source_timestamp_ns`. Leave unset for
  /// static authored geometry. When set, `source_timestamp_ns` MUST also be
  /// set, non-negative, and this budget MUST be positive.
  std::optional<std::int64_t> source_freshness_budget_ns;

  /// Geometries making up this body. Ordering is preserved by the
  /// builder; engines MUST treat the list as a multi-set keyed by
  /// `body_id + index`.
  std::vector<CollisionGeometryDescriptor> geometries;

  /// Per-body content hash over (body_id, owner_kind, parent_body_id,
  /// frame_id, pose, geometries source-hash digest). Set by the
  /// builder; publishers SHOULD NOT pre-populate this field — the
  /// builder will overwrite it.
  std::string body_content_hash;
};

/// Pose of one component-owned model frame in the collision-world frame.
///
/// `CollisionWorldSnapshot` bodies are published in the scene/world frame, but
/// most robot collision engines evaluate Pinocchio-style robot geometry in the
/// robot model's root frame. This descriptor is the explicit bridge between
/// those frames: engines use it to transform scene resources and runtime
/// objects into the model frame before running per-waypoint FK.
struct CollisionComponentModelFrame {
  ControllableComponentId controllable_component_id;
  ModelInstanceId model_instance_id;
  FrameRoleId frame_role_id;
  /// Compiler-owned frame name retained only as internal projection evidence.
  std::string compiled_frame_name;
  ModelRevision model_revision;

  /// Pose of `compiled_frame_name` in the collision-world frame.
  Pose3DTO world_pose;
};

/// Immutable snapshot of the active collision world (spec §3.8.1,
/// defined by the kernel-architecture sprint P-2-impl-snapshot
/// chunk).
///
/// The snapshot is the *world* half of the (world, policy) pair that
/// every `ICollisionQueryEngine` worker consumes. It carries the
/// scene state — component-model bodies, scene resources, runtime objects,
/// their poses, and their geometry descriptors —
/// plus a deterministic revision identity that engines use for cache
/// invalidation and that the kernel uses for mismatch correlation.
///
/// **What the snapshot does NOT carry:**
///   - Per-query robot joint state (that is `CollisionQuery::robot_state`).
///   - Policy state — allowed pairs, support-contact entries, full-
///     diagnostic requirements (that is `CollisionPolicySnapshot`).
///     The policy resolver derives its allowed-pair lists from the
///     active snapshot; the snapshot itself does NOT publish them.
///
/// **Threading.** A snapshot is immutable after `build()`. Callers
/// share it across N engine workers via
/// `std::shared_ptr<const CollisionWorldSnapshot>` (see
/// `CollisionWorldSnapshotPtr`). Concurrent read access is safe; no
/// synchronization is required.
///
/// **Determinism.** Two snapshots produced from identical world
/// state by identical publishers MUST produce identical
/// `revision.content_hash`. The builder enforces this by sorting all
/// internal collections and mixing fields in canonical order.
struct CollisionWorldSnapshot {
  /// Format version. Bumped only for backward-incompatible schema
  /// changes. Engines that don't recognize the version MUST fail
  /// closed (`CollisionEngineStatus::kDiagnosticIncomplete` with a
  /// `snapshot_format_version` diagnostic key); the kernel SHOULD
  /// refuse to dispatch under an unknown version.
  std::uint32_t snapshot_format_version = 1;

  /// Identifier of the publisher that assembled this snapshot, e.g.
  /// `"roboplan"`, `"isaac_observer"`, `"deterministic_fixture"`.
  /// Required (non-empty); the builder rejects empty publisher ids.
  std::string publisher_id;

  /// Snapshot identity. `revision_id` is monotonic-per-publisher and
  /// `content_hash` is the deterministic digest produced by the
  /// builder. Engines compare `revision_id` for cheap cache-hit
  /// checks and fall back to `content_hash` for cross-publisher
  /// reconciliation. Both fields are populated by the builder; do
  /// not set them by hand.
  CollisionWorldRevision revision;

  /// Steady-clock wall time the snapshot was assembled, ns. Used by
  /// telemetry only; engines MUST NOT use it for cache decisions.
  std::int64_t emitted_at_ns = 0;

  /// Canonical component/model/frame projection rows needed by collision
  /// engines to transform scene resources into each model frame.
  std::vector<CollisionComponentModelFrame> component_model_frames;

  /// All collision bodies, sorted lexicographically by `body_id`.
  /// Sorting is performed by the builder; publishers MUST NOT rely
  /// on a particular ordering surviving `build()`.
  std::vector<CollisionWorldBody> bodies;

  /// Pre-computed counts. The builder populates them; engines and
  /// telemetry MUST read these instead of scanning `bodies` (per the
  /// no-hot-path-scene-scan rule). `geometry_count` is the sum over
  /// `bodies[i].geometries.size()`.
  std::int64_t body_count = 0;
  std::int64_t geometry_count = 0;
  std::int64_t component_model_body_count = 0;
  std::int64_t scene_resource_count = 0;
  std::int64_t runtime_object_count = 0;

  /// True iff any geometry in the snapshot reports
  /// `geometry_kind == kTriangleMesh` or `kCompound`. Engines that
  /// fail closed on mesh inputs can short-circuit at ingestion time
  /// without scanning every geometry; production qualification on
  /// mesh-bearing profiles MUST gate on this flag.
  bool contains_mesh_geometry = false;

  /// True iff every body's `geometries` is non-empty. The builder
  /// guarantees this is true for any snapshot it returns; callers
  /// that construct a snapshot by hand for tests SHOULD set this
  /// honestly.
  bool all_bodies_have_geometry = true;
};

/// RAII pointer alias for snapshots shared across workers.
using CollisionWorldSnapshotPtr = std::shared_ptr<const CollisionWorldSnapshot>;

}  // namespace manyforge::core
