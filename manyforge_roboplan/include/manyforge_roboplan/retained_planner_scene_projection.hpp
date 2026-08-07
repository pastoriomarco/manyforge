#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include <manyforge_core/collision/i_collision_world_snapshot_publisher.hpp>
#include <manyforge_core/ports/iplanner_scene_projection.hpp>
#include <manyforge_core/types/retained_product.hpp>
#include <manyforge_roboplan/retained_collision_world_publisher.hpp>

namespace manyforge::roboplan_backend {

struct RetainedPlannerSceneProjectionConfig final {
  std::string publisher_id;
  std::size_t transaction_replay_capacity{256U};
  std::size_t collision_ledger_capacity{
      RetainedCollisionWorldPublisher::kDefaultLedgerCapacity};
};

/// One exact host-owned native projection state machine.
///
/// The logical scene remains externally owned. This adapter accepts only
/// owner-normalized transactions, verifies their deterministic successor
/// snapshot before effects, and publishes an immutable collision projection.
/// It also acts as the stable publisher handle used by a runtime composition,
/// so explicit rebuild may replace the concrete publisher without leaving
/// consumers attached to a stale instance.
class RetainedPlannerSceneProjection final
    : public core::IPlannerSceneProjection,
      public core::ICollisionWorldSnapshotPublisher {
public:
  using Clock = std::function<std::int64_t()>;

  [[nodiscard]] static core::Result<
      std::shared_ptr<RetainedPlannerSceneProjection>>
  CreateInitialized(
      RetainedCollisionWorldProjector projector,
      core::RetainedProductDTO planning_collision_model,
      core::ProjectionNativeRuntimeBindingDTO native_runtime_binding,
      core::LogicalSceneSnapshotDTO initial_scene,
      RetainedPlannerSceneProjectionConfig config, Clock clock);

  [[nodiscard]] static core::Result<
      std::shared_ptr<RetainedPlannerSceneProjection>>
  CreateUninitialized(
      RetainedCollisionWorldProjector projector,
      core::RetainedProductDTO planning_collision_model,
      core::ProjectionNativeRuntimeBindingDTO native_runtime_binding,
      RetainedPlannerSceneProjectionConfig config, Clock clock);

  [[nodiscard]] core::ProjectionCommitResultDTO
  ApplyTransaction(const core::SceneTransactionDTO &transaction) override;
  [[nodiscard]] core::Result<core::CollisionWorldLeaseInventory>
  RetainProjection(const core::ProjectionRevisionDTO &projection_revision,
                   core::ProjectionLeaseId projection_lease_id) override;
  void ReleaseProjection(
      const core::ProjectionLeaseId &projection_lease_id) override;
  [[nodiscard]] core::ProjectionHealthDTO ProjectionHealth() const override;
  [[nodiscard]] core::ProjectionCommitResultDTO
  RebuildProjection(const core::NativeProjectionRebuildDTO &request) override;

  [[nodiscard]] std::shared_ptr<const core::LogicalSceneSnapshotDTO>
  logical_snapshot() const;
  [[nodiscard]] const collision::ManyForgeCollisionEngine::SharedModelPtr &
  collision_shared_model() const noexcept;

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
  void release_scene_snapshot_lease_inventory(
      const core::ProjectionLeaseId &projection_lease_id) override;
  [[nodiscard]] core::CollisionWorldLeaseRetentionStats
  scene_snapshot_lease_retention_stats() const override;
  [[nodiscard]] core::RuntimeCollisionQueryBundlePtr
  published_runtime_collision_query_bundle() const override;

private:
  struct TransactionRecord final {
    core::SceneTransactionDTO transaction;
    core::ProjectionCommitResultDTO result;
  };

  RetainedPlannerSceneProjection(
      RetainedCollisionWorldProjector projector,
      core::RetainedProductDTO planning_collision_model,
      core::ProjectionNativeRuntimeBindingDTO native_runtime_binding,
      RetainedPlannerSceneProjectionConfig config, Clock clock);

  [[nodiscard]] core::Result<std::shared_ptr<RetainedCollisionWorldPublisher>>
  BuildPublisher(const core::LogicalSceneSnapshotDTO &snapshot,
                 core::CollisionWorldMutationSource source,
                 std::string source_label) const;
  [[nodiscard]] core::ProjectionCommitResultDTO
  RollbackProven(std::optional<std::uint64_t> transaction_sequence,
                 const core::ProjectionRevisionDTO &proposed,
                 core::ErrorDTO error);
  [[nodiscard]] core::ProjectionCommitResultDTO
  Desynchronize(std::optional<std::uint64_t> transaction_sequence,
                const core::ProjectionRevisionDTO &proposed,
                core::ErrorDTO error);
  [[nodiscard]] bool
  HealthyAt(const core::ProjectionRevisionDTO &revision) const noexcept;

  RetainedCollisionWorldProjector projector_;
  core::RetainedProductDTO planning_collision_model_;
  core::ProjectionNativeRuntimeBindingDTO native_runtime_binding_;
  RetainedPlannerSceneProjectionConfig config_;
  Clock clock_;

  mutable std::mutex mutex_;
  core::ProjectionHealthDTO health_;
  std::shared_ptr<const core::LogicalSceneSnapshotDTO> logical_snapshot_;
  std::map<std::uint64_t, TransactionRecord> transaction_records_;
  std::map<core::ProjectionLeaseId,
           std::shared_ptr<RetainedCollisionWorldPublisher>>
      retained_lease_publishers_;
  std::deque<core::ProjectionLeaseId> retained_lease_order_;
  std::atomic<std::shared_ptr<RetainedCollisionWorldPublisher>>
      active_publisher_;
};

} // namespace manyforge::roboplan_backend
