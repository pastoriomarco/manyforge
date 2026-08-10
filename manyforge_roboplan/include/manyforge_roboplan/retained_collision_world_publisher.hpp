#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <manyforge_core/collision/i_collision_world_snapshot_publisher.hpp>
#include <manyforge_core/types/error_code.hpp>
#include <manyforge_core/types/logical_scene_snapshot.hpp>
#include <manyforge_roboplan/retained_collision_world_projector.hpp>

namespace manyforge::roboplan_backend {

/// Provenance for one already-accepted logical-scene publication.
struct RetainedCollisionWorldPublicationContext final {
  core::CollisionWorldMutationSource mutation_source =
      core::CollisionWorldMutationSource::kSceneEdit;
  std::vector<core::CollisionBodyId> affected_body_ids;
  std::int64_t observed_timestamp_ns = 0;
  core::RequestAttributionDTO attribution;
  std::string source_label;
  std::optional<std::string> correlation_id;
};

/// Authoritative immutable collision-world projection over one exact retained
/// model generation and logical-scene lineage.
///
/// Scene ownership remains outside this class. A successful publish consumes
/// one owner-issued, content-verified snapshot, resolves its effective policy,
/// prepares the runtime query indexes, and atomically swaps one coherent
/// immutable bundle. Failed projection leaves the prior publication intact.
class RetainedCollisionWorldPublisher final
    : public core::ICollisionWorldSnapshotPublisher {
public:
  using Clock = std::function<std::int64_t()>;

  static constexpr std::size_t kDefaultLedgerCapacity = 32U;
  static constexpr std::size_t kRuntimeRetirementCapacity = 64U;

  [[nodiscard]] static core::Result<
      std::shared_ptr<RetainedCollisionWorldPublisher>>
  create(RetainedCollisionWorldProjector projector, std::string publisher_id,
         const core::LogicalSceneSnapshotDTO &initial_scene,
         RetainedCollisionWorldPublicationContext initial_context, Clock clock,
         std::size_t ledger_capacity = kDefaultLedgerCapacity);

  [[nodiscard]] core::Result<core::CollisionWorldRevision>
  publish(const core::LogicalSceneSnapshotDTO &logical_scene,
          RetainedCollisionWorldPublicationContext context);

  /// Reserve one process-local collision-world revision for an immutable
  /// owner-private candidate. The candidate is not published, but consuming
  /// the revision keeps shared collision-engine caches and later durable
  /// publications in one monotonic identity domain.
  [[nodiscard]] core::Result<std::uint64_t>
  reserve_private_collision_world_revision();

  [[nodiscard]] std::vector<core::CollisionWorldLedgerEntry>
  recent_ledger(std::size_t limit = 0U) const;
  [[nodiscard]] const collision::ManyForgeCollisionEngine::SharedModelPtr &
  collision_shared_model() const noexcept {
    return projector_.shared_model();
  }

  [[nodiscard]] core::CollisionWorldSnapshotPtr snapshot() const override;
  [[nodiscard]] core::CollisionWorldRevision
  published_collision_world_revision() const override;
  [[nodiscard]] std::optional<core::CollisionPolicySnapshot>
  collision_policy_snapshot() const override;
  [[nodiscard]] core::CollisionWorldPolicySnapshotBundle
  collision_world_policy_snapshot_bundle() const override;
  [[nodiscard]] std::optional<core::CollisionWorldLeaseInventory>
  current_scene_snapshot_lease_inventory() const override;
  [[nodiscard]] std::optional<core::CollisionWorldLeaseInventory>
  retain_scene_snapshot_lease_inventory(
      core::ProjectionLeaseId projection_lease_id = core::ProjectionLeaseId{
          std::string{}}) override;
  /// Retain with an owner-captured monotonic timestamp without reading the
  /// publisher clock a second time. This changes only lease age; per-object
  /// source timestamps remain immutable projection evidence.
  [[nodiscard]] std::optional<core::CollisionWorldLeaseInventory>
  retain_scene_snapshot_lease_inventory_at(
      core::ProjectionLeaseId projection_lease_id, std::int64_t captured_at_ns);
  void release_scene_snapshot_lease_inventory(
      const core::ProjectionLeaseId &projection_lease_id) override;
  [[nodiscard]] core::CollisionWorldLeaseRetentionStats
  scene_snapshot_lease_retention_stats() const override;
  [[nodiscard]] core::RuntimeCollisionQueryBundlePtr
  published_runtime_collision_query_bundle() const override;

private:
  RetainedCollisionWorldPublisher(RetainedCollisionWorldProjector projector,
                                  std::string publisher_id, Clock clock,
                                  std::size_t ledger_capacity);

  [[nodiscard]] std::optional<core::CollisionWorldLeaseInventory>
  retain_scene_snapshot_lease_inventory_locked(
      core::ProjectionLeaseId projection_lease_id, std::int64_t captured_at_ns);

  [[nodiscard]] core::Result<core::CollisionWorldRevision>
  publish_locked(const core::LogicalSceneSnapshotDTO &logical_scene,
                 RetainedCollisionWorldPublicationContext context);
  [[nodiscard]] bool
  retire_and_swap_locked(core::RuntimeCollisionQueryBundlePtr next);

  RetainedCollisionWorldProjector projector_;
  std::string publisher_id_;
  Clock clock_;
  std::size_t ledger_capacity_;

  mutable std::mutex writer_mutex_;
  std::deque<core::CollisionWorldLedgerEntry> ledger_;
  std::optional<core::LogicalSceneSnapshotIdentityDTO> last_scene_identity_;
  std::optional<core::CollisionWorldLeaseInventory> current_inventory_;
  std::map<core::ProjectionLeaseId, core::CollisionWorldLeaseInventory>
      retained_inventories_;
  std::deque<core::ProjectionLeaseId> retained_inventory_order_;
  std::uint64_t next_lease_id_ = 1U;
  std::uint64_t collision_world_revision_sequence_ = 0U;
  std::uint64_t world_revision_id_ = 0U;
  std::uint64_t policy_revision_id_ = 0U;
  std::uint64_t owner_sequence_ = 0U;
  std::uint64_t runtime_epoch_ = 0U;
  std::string policy_content_hash_;

  std::atomic<core::RuntimeCollisionQueryBundlePtr> runtime_bundle_;
  std::array<core::RuntimeCollisionQueryBundlePtr,
             kRuntimeRetirementCapacity>
      retired_runtime_bundles_{};
};

}  // namespace manyforge::roboplan_backend
