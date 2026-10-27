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
#include <thread>

namespace manyforge::roboplan_backend {

struct RetainedPlannerSceneProjectionConfig final {
  std::string publisher_id;
  std::size_t transaction_replay_capacity{256U};
};

/// One exact host-owned native projection state machine.
///
/// The logical scene remains externally owned. This adapter accepts only
/// owner-normalized transactions and verifies their deterministic successor
/// snapshot before effects. The collision world is a lazily derived cache of
/// the latest logical scene: a commit only records the scene and advances a
/// change counter; the world, policy and runtime bundle are rebuilt when a
/// consumer is about to compute collisions (planning, validation, the
/// runtime monitor) and the scene changed since the last build. It also acts
/// as the stable publisher handle used by a runtime composition, so explicit
/// rebuild may replace the concrete publisher without leaving consumers
/// attached to a stale instance.
class RetainedPlannerSceneProjection final
    : public core::IPlannerSceneProjection,
      public core::ICollisionWorldSnapshotPublisher {
public:
  using Clock = std::function<std::int64_t()>;

  ~RetainedPlannerSceneProjection() override;
  RetainedPlannerSceneProjection(const RetainedPlannerSceneProjection &) =
      delete;
  RetainedPlannerSceneProjection &
  operator=(const RetainedPlannerSceneProjection &) = delete;

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
  [[nodiscard]] core::Result<core::CollisionWorldLeaseInventory>
  RetainCurrentProjection(core::ProjectionLeaseId projection_lease_id) override;

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

  [[nodiscard]] std::shared_ptr<const core::LogicalSceneSnapshotDTO>
  RetainedScene(const core::ProjectionLeaseBindingDTO &binding) const;

  /// Bounds the projection leases held for local and remote planners; a
  /// leaked lease cannot grow memory without limit.
  static constexpr std::size_t kMaxRetainedProjectionLeases = 64U;

  RetainedPlannerSceneProjection(
      RetainedCollisionWorldProjector projector,
      core::RetainedProductDTO planning_collision_model,
      core::ProjectionNativeRuntimeBindingDTO native_runtime_binding,
      RetainedPlannerSceneProjectionConfig config, Clock clock,
      std::shared_ptr<const RetainedSceneFrameResolver> frame_resolver);

  /// Rebuild the collision world from the latest logical scene if it changed
  /// since the last build. Runs under mutex_ on the single publication writer;
  /// a failed build desynchronizes the projection (consumers fail closed).
  void BuildCollisionWorldLocked();
  /// NRT consumer entry: bring the collision world up to date before use.
  void RefreshCollisionWorld() const;
  /// Runtime-monitor entry: never builds; asks the build worker to rebuild
  /// off the real-time thread when the scene changed.
  void RequestCollisionWorldBuild() const noexcept;
  void RunCollisionWorldBuilds();

  [[nodiscard]] core::Result<std::shared_ptr<RetainedCollisionWorldPublisher>>
  BuildPublisher(const RetainedCollisionWorldProjector &projector,
                 const core::LogicalSceneSnapshotDTO &snapshot) const;
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
  [[nodiscard]] core::Result<core::CollisionWorldLeaseInventory>
  RetainProjectionLocked(const core::ProjectionRevisionDTO &revision,
                         core::ProjectionLeaseId lease_id);
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
  /// Retained inventories by lease id: the exact scene and world a planner
  /// started from. Their handles stay valid across later publications.
  std::map<core::ProjectionLeaseId, core::CollisionWorldLeaseInventory>
      retained_lease_publishers_;
  std::deque<core::ProjectionLeaseId> retained_lease_order_;
  std::atomic<std::shared_ptr<RetainedCollisionWorldPublisher>>
      active_publisher_;
  /// Change notification only: commits advance scene_change_ and a build
  /// records the change it covered in built_change_. Never a precondition.
  std::atomic<std::uint64_t> scene_change_{0U};
  std::atomic<std::uint64_t> built_change_{0U};
  mutable std::atomic<std::uint64_t> requested_change_{0U};
  mutable std::atomic<std::uint64_t> build_wake_{0U};
  std::atomic<bool> stopping_{false};
  /// Off-real-time builder woken by the runtime monitor. Declared last so it
  /// starts after, and is joined before, every member it touches.
  std::thread build_worker_;
};

} // namespace manyforge::roboplan_backend
