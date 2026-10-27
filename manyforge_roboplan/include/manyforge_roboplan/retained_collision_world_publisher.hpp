#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <manyforge_core/collision/i_collision_world_snapshot_publisher.hpp>
#include <manyforge_core/types/error_code.hpp>
#include <manyforge_core/types/logical_scene_snapshot.hpp>
#include <manyforge_roboplan/retained_collision_world_projector.hpp>

namespace manyforge::roboplan_backend {

/// Converts the latest logical scene into collision bodies and rules over one
/// retained model generation, and publishes the result atomically.
///
/// Scene ownership remains outside this class. A successful publish projects
/// the world (carried objects parented to their gripper body), resolves the
/// effective policy, prepares the runtime query indexes, and swaps one
/// immutable bundle. A failed projection leaves the prior publication intact.
/// Consumers always read the latest bundle.
class RetainedCollisionWorldPublisher final
    : public core::ICollisionWorldSnapshotPublisher {
public:
  using Clock = std::function<std::int64_t()>;

  [[nodiscard]] static core::Result<
      std::shared_ptr<RetainedCollisionWorldPublisher>>
  create(RetainedCollisionWorldProjector projector, std::string publisher_id,
         const core::LogicalSceneSnapshotDTO &initial_scene,
         std::int64_t observed_timestamp_ns, Clock clock);

  [[nodiscard]] core::Result<core::CollisionWorldRevision>
  publish(const core::LogicalSceneSnapshotDTO &logical_scene,
          std::int64_t observed_timestamp_ns);

  /// Reserve one process-local collision-world revision for an immutable
  /// owner-private candidate. The candidate is not published, but consuming
  /// the revision keeps the shared collision-engine scene cache, keyed by
  /// publisher id and revision, free of two contents under one revision.
  [[nodiscard]] core::Result<std::uint64_t>
  reserve_private_collision_world_revision();

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
  /// The latest inventory tagged with the lease id and the publisher clock.
  /// Nothing is retained here; the returned handles stay valid on their own.
  [[nodiscard]] std::optional<core::CollisionWorldLeaseInventory>
  retain_scene_snapshot_lease_inventory(
      core::ProjectionLeaseId projection_lease_id = core::ProjectionLeaseId{
          std::string{}}) override;
  [[nodiscard]] core::RuntimeCollisionQueryBundlePtr
  published_runtime_collision_query_bundle() const override;

private:
  RetainedCollisionWorldPublisher(RetainedCollisionWorldProjector projector,
                                  std::string publisher_id, Clock clock);

  void swap_locked(core::RuntimeCollisionQueryBundlePtr next);

  RetainedCollisionWorldProjector projector_;
  std::string publisher_id_;
  Clock clock_;

  mutable std::mutex writer_mutex_;
  std::optional<core::CollisionWorldLeaseInventory> current_inventory_;
  std::uint64_t next_lease_id_ = 1U;
  std::uint64_t collision_world_revision_sequence_ = 0U;
  std::uint64_t policy_revision_id_ = 0U;
  std::string policy_content_hash_;

  std::atomic<core::RuntimeCollisionQueryBundlePtr> runtime_bundle_;
  /// Superseded bundles still referenced by a reader. Holding them here keeps
  /// their last-owner destruction on this NRT writer instead of a runtime
  /// monitor tick; entries no reader holds are dropped on the next publish.
  std::vector<core::RuntimeCollisionQueryBundlePtr> retired_runtime_bundles_;
};

}  // namespace manyforge::roboplan_backend
