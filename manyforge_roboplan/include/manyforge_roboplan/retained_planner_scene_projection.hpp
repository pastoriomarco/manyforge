#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <manyforge_core/collision/i_collision_world_snapshot_publisher.hpp>
#include <manyforge_core/ports/iplanner_scene_projection.hpp>
#include <manyforge_core/types/retained_product.hpp>
#include <manyforge_roboplan/retained_collision_world_publisher.hpp>
#include <manyforge_roboplan/retained_scene_frame_resolver.hpp>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

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
      RetainedPlannerSceneProjectionConfig config, Clock clock,
      std::shared_ptr<const RetainedSceneFrameResolver> frame_resolver =
          nullptr);

  [[nodiscard]] static core::Result<
      std::shared_ptr<RetainedPlannerSceneProjection>>
  CreateUninitialized(
      RetainedCollisionWorldProjector projector,
      core::RetainedProductDTO planning_collision_model,
      core::ProjectionNativeRuntimeBindingDTO native_runtime_binding,
      RetainedPlannerSceneProjectionConfig config, Clock clock,
      std::shared_ptr<const RetainedSceneFrameResolver> frame_resolver =
          nullptr);

  /// Install the direct-provider resolver assembled by the exact active
  /// physical composition. The caller performs this once while command
  /// admission is closed, after candidate participants are installed.
  [[nodiscard]] core::Result<void> InstallActiveFrameResolver(
      const core::ProjectionRevisionDTO &expected_active_revision,
      std::shared_ptr<const RetainedSceneFrameResolver> frame_resolver);

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
  [[nodiscard]] core::NativeProjectionBootstrapRebuildResultDTO
  RebuildBootstrapProjection(
      const core::NativeProjectionBootstrapRebuildDTO &request) override;
  [[nodiscard]] core::NativeProjectionBootstrapStatusDTO
  BootstrapProjectionStatus(const core::NativeProjectionBootstrapStatusQueryDTO
                                &query) const override;
  [[nodiscard]] core::Result<core::SceneFrameResolutionReceiptDTO>
  ResolveSceneFrame(
      const core::SceneFrameResolutionQueryDTO &query) const override;
  [[nodiscard]] core::Result<core::SceneFrameResolutionReceiptDTO>
  ResolveCandidateSceneFrame(
      const core::CandidateSceneFrameResolutionQueryDTO &query) const override;
  [[nodiscard]] core::Result<core::CollisionWorldLeaseInventory>
  ProjectSceneCandidate(
      const core::SceneCandidatePreviewDTO &candidate,
      const core::ProjectionLeaseBindingDTO &predecessor_binding,
      core::ProjectionLeaseId candidate_lease_id) const override;
  [[nodiscard]] core::Result<core::CollisionPolicySnapshot>
  ProjectEffectiveCollisionPolicy(
      const core::LogicalSceneSnapshotDTO &logical_scene,
      std::uint64_t effective_policy_revision_id) const override;

  [[nodiscard]] std::shared_ptr<const core::LogicalSceneSnapshotDTO>
  logical_snapshot() const;
  [[nodiscard]] collision::ManyForgeCollisionEngine::SharedModelPtr
  collision_shared_model() const;

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

  struct BootstrapRecord final {
    core::BootstrapId bootstrap_id;
    core::BootstrapGeneration bootstrap_generation;
    core::ProjectionRevisionDTO target;
  };

  enum class RebuildConstructionContext {
    kOrdinary,
    kBootstrap,
  };

  struct RetainedProjectionLease final {
    std::shared_ptr<RetainedCollisionWorldPublisher> publisher;
    std::int64_t captured_at_ns{};
  };

  RetainedPlannerSceneProjection(
      RetainedCollisionWorldProjector projector,
      core::RetainedProductDTO planning_collision_model,
      core::ProjectionNativeRuntimeBindingDTO native_runtime_binding,
      RetainedPlannerSceneProjectionConfig config, Clock clock,
      std::shared_ptr<const RetainedSceneFrameResolver> frame_resolver);

  [[nodiscard]] core::Result<std::shared_ptr<RetainedCollisionWorldPublisher>>
  BuildPublisher(const RetainedCollisionWorldProjector &projector,
                 const core::LogicalSceneSnapshotDTO &snapshot,
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
  [[nodiscard]] core::ProjectionCommitResultDTO
  RebuildProjectionLocked(
      const core::NativeProjectionRebuildDTO &request,
      RebuildConstructionContext construction_context);

  RetainedCollisionWorldProjector projector_;
  core::RetainedProductDTO planning_collision_model_;
  std::shared_ptr<const RetainedSceneFrameResolver> frame_resolver_;
  core::ProjectionNativeRuntimeBindingDTO native_runtime_binding_;
  RetainedPlannerSceneProjectionConfig config_;
  Clock clock_;
  // BuildPublisher is called during construction or under mutex_. Each
  // concrete publisher restarts its revision sequence, so it must also have
  // a distinct identity within this projection lifetime.
  mutable std::uint64_t next_publisher_incarnation_{1U};

  mutable std::mutex mutex_;
  core::ProjectionHealthDTO health_;
  std::shared_ptr<const core::LogicalSceneSnapshotDTO> logical_snapshot_;
  std::map<std::uint64_t, TransactionRecord> transaction_records_;
  std::optional<BootstrapRecord> bootstrap_record_;
  std::optional<core::NativeProjectionRebuildDTO> maintenance_successor_;
  std::map<core::ProjectionLeaseId, RetainedProjectionLease>
      retained_lease_publishers_;
  std::deque<core::ProjectionLeaseId> retained_lease_order_;
  std::atomic<std::shared_ptr<RetainedCollisionWorldPublisher>>
      active_publisher_;
};

} // namespace manyforge::roboplan_backend
