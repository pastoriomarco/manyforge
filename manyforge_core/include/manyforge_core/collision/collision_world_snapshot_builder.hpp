#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <manyforge_core/types/collision_world_snapshot.hpp>
#include <manyforge_core/types/error_code.hpp>

namespace manyforge::core {

/// Build a deterministic, validated `CollisionWorldSnapshot` (spec
/// §3.8.1, defined by the kernel-architecture sprint
/// P-2-impl-snapshot chunk).
///
/// The builder is the single canonical way to assemble a snapshot.
/// Publishers MUST go through it; hand-constructed snapshots are only
/// acceptable in unit tests that deliberately probe builder
/// invariants from the outside.
///
/// **Invariants the builder enforces** (failure → `Result::Err`):
///   - `publisher_id` is non-empty.
///   - `snapshot_format_version` is non-zero.
///   - `revision_id` is non-zero (zero is reserved for "unset").
///   - `emitted_at_ns` is non-negative.
///   - Each body's `body_id` is non-empty.
///   - Each body's `body_id` matches its `owner_kind` namespace.
///   - Each body's `frame_id` is non-empty.
///   - Body ids are unique within the snapshot.
///   - Each body has at least one geometry.
///   - Each body's geometries reference the same `body_id` as the
///     body they live on.
///   - Each body's geometries use the same `owner_kind` as the body.
///   - Primitive geometries carry positive finite dimensions for their
///     kind and no unrelated primitive dimensions; mesh / compound
///     geometries carry `source_uri` or `source_hash` and no primitive
///     dimensions.
///   - Each attached-object body declares a non-empty `parent_link`.
///   - Each attached-object `parent_link` has `robot/<id>/link/<name>`
///     shape (no `/collision/<idx>` suffix).
///   - Each non-attached body does NOT declare a `parent_link`.
///
/// **Deterministic outputs:**
///   - Bodies are sorted lexicographically by `body_id` after
///     validation.
///   - Each body's `body_content_hash` is computed by the builder
///     using `ContentHasher` over (body_id, owner_kind, parent_link,
///     frame_id, pose, geometries' source-defining fields).
///   - The snapshot's `revision.content_hash` is the digest over
///     `publisher_id`, `snapshot_format_version`, `revision_id`,
///     and the sorted bodies' `body_content_hash` values.
///   - Cached counts (`body_count`, `geometry_count`,
///     `robot_link_count`, etc.) are populated from the sorted body
///     list.
///   - `contains_mesh_geometry` and `all_bodies_have_geometry` are
///     populated honestly from the geometry list.
///
/// **What the builder does NOT do:**
///   - It does not resolve allowed-pair lists. Those are policy state
///     and live in `CollisionPolicySnapshot`.
///   - It does not validate geometry kinds against engine capability.
///     Engines fail closed at ingestion time.
///   - It does not normalize poses (no quaternion re-normalization, no
///     pose-frame transform). Publishers are responsible for poses
///     they hand in.
class CollisionWorldSnapshotBuilder {
 public:
  CollisionWorldSnapshotBuilder() = default;

  CollisionWorldSnapshotBuilder& set_publisher_id(std::string id);
  CollisionWorldSnapshotBuilder& set_snapshot_format_version(
      std::uint32_t version);
  CollisionWorldSnapshotBuilder& set_revision_id(std::uint64_t revision_id);
  CollisionWorldSnapshotBuilder& set_emitted_at_ns(std::int64_t emitted_at_ns);

  /// Append a body. Bodies are validated and sorted at `build()`
  /// time, not at `add_body()` time, so callers may add bodies in
  /// any order.
  CollisionWorldSnapshotBuilder& add_body(CollisionWorldBody body);

  /// Assemble + validate + hash + return.
  ///
  /// On success: the returned snapshot is fully populated, sorted,
  /// hashed, and ready to be wrapped in
  /// `std::make_shared<const CollisionWorldSnapshot>(...)`.
  ///
  /// On failure: `Result::Err` with `ErrorCode::kInvariantViolation`
  /// and an `ErrorDTO::detail` carrying a human-readable explanation.
  /// The builder is left in an unspecified state; do not reuse it
  /// after a failed `build()`.
  [[nodiscard]] Result<CollisionWorldSnapshot> build();

  /// Convenience wrapper that wraps the built snapshot in a
  /// `shared_ptr<const>` for direct hand-off to engine workers.
  [[nodiscard]] Result<CollisionWorldSnapshotPtr> build_shared();

 private:
  std::string publisher_id_;
  std::uint32_t snapshot_format_version_ = 1;
  std::uint64_t revision_id_ = 0;
  std::int64_t emitted_at_ns_ = 0;
  std::vector<CollisionWorldBody> bodies_;
};

}  // namespace manyforge::core
