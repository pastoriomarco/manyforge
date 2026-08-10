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

  /// Preload one exact candidate structural model while the current
  /// projection remains active. This is a host-internal staged-ready step,
  /// not part of the logical-scene projection port: StageProjection still
  /// consumes only the owner-issued frozen scene and retained product
  /// identity and never resolves deployment artifacts itself.
  [[nodiscard]] core::Result<void> PrepareCandidateProjection(
      RetainedCollisionWorldProjector candidate_projector,
      core::RetainedProductDTO candidate_planning_collision_model,
      std::shared_ptr<const RetainedSceneFrameResolver>
          candidate_frame_resolver = nullptr);

  /// Replace the command-fenced bootstrap resolver with the direct-provider
  /// resolver assembled by the exact active physical composition. The caller
  /// performs this once while the Apply configuration fence is held, after
  /// candidate participants are installed and before command admission opens.
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
  [[nodiscard]] core::NativeProjectionStageResultDTO
  StageProjection(const core::NativeProjectionStageDTO &request) override;
  [[nodiscard]] core::NativeProjectionActivationResultDTO
  ActivateStagedProjection(
      const core::NativeProjectionActivationDTO &request) override;
  [[nodiscard]] core::NativeProjectionRetireResultDTO
  RetireProjection(const core::NativeProjectionRetireDTO &request) override;
  [[nodiscard]] core::NativeProjectionRetireStatusDTO ProjectionRetireStatus(
      const core::NativeProjectionRetireStatusQueryDTO &query) const override;
  [[nodiscard]] core::NativeProjectionDiscardResultDTO DiscardStagedProjection(
      const core::NativeProjectionDiscardDTO &request) override;
  [[nodiscard]] core::NativeProjectionStatusDTO ProjectionStageStatus(
      const core::NativeProjectionStatusQueryDTO &query) const override;
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

  struct StageRecord final {
    core::NativeProjectionStageDTO request;
    RetainedCollisionWorldProjector candidate_projector;
    core::RetainedProductDTO candidate_planning_collision_model;
    std::shared_ptr<const RetainedSceneFrameResolver> candidate_frame_resolver;
    std::shared_ptr<RetainedCollisionWorldPublisher> candidate_publisher;
    core::NativeProjectionStageResultDTO stage_result;
    bool activated{};
    bool discarded{};
  };

  struct RetireRecord final {
    core::NativeProjectionRetireDTO request;
    core::NativeProjectionRetireResultDTO result;
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
  RebuildProjectionLocked(const core::NativeProjectionRebuildDTO &request);

  RetainedCollisionWorldProjector projector_;
  core::RetainedProductDTO planning_collision_model_;
  std::shared_ptr<const RetainedSceneFrameResolver> frame_resolver_;
  struct PreparedCandidateModel final {
    RetainedCollisionWorldProjector projector;
    core::RetainedProductDTO planning_collision_model;
    std::shared_ptr<const RetainedSceneFrameResolver> frame_resolver;
  };
  std::optional<PreparedCandidateModel> prepared_candidate_model_;
  core::ProjectionNativeRuntimeBindingDTO native_runtime_binding_;
  RetainedPlannerSceneProjectionConfig config_;
  Clock clock_;

  mutable std::mutex mutex_;
  core::ProjectionHealthDTO health_;
  std::shared_ptr<const core::LogicalSceneSnapshotDTO> logical_snapshot_;
  std::map<std::uint64_t, TransactionRecord> transaction_records_;
  std::optional<BootstrapRecord> bootstrap_record_;
  std::optional<StageRecord> stage_record_;
  std::optional<RetireRecord> retire_record_;
  std::map<core::ProjectionLeaseId,
           std::shared_ptr<RetainedCollisionWorldPublisher>>
      retained_lease_publishers_;
  std::deque<core::ProjectionLeaseId> retained_lease_order_;
  std::atomic<std::shared_ptr<RetainedCollisionWorldPublisher>>
      active_publisher_;
};

} // namespace manyforge::roboplan_backend
