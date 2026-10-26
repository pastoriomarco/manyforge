#pragma once

#include "execution_host_config.hpp"
#include <manyforge_core/ports/iscene_authority.hpp>
#include <manyforge_core/ports/iscene_candidate_preview.hpp>
#include <manyforge_core/ports/iplanning_scene_synchronizer.hpp>
#include <manyforge_core/collision/i_collision_world_snapshot_publisher.hpp>
#include <stop_token>
#include <manyforge_core/ports/iplanner_scene_projection.hpp>

namespace manyforge::roboplan_backend::host_detail {

struct ManagerSceneRead final {
  core::RequestOwnerEpoch request_owner_epoch;
  core::ProjectionRevisionDTO projection_revision;
  std::shared_ptr<const core::LogicalSceneSnapshotDTO> logical_scene;
};

/// Process-local adapter to the sole Manager-owned logical-scene reducer.
/// It carries exact owner DTOs over the existing same-user command socket and
/// stores no scene, replay, or projection authority of its own.
class ManagerSceneAuthorityProxy final : public core::ISceneAuthority,
                                         public core::ISceneCandidatePreview {
public:
  ManagerSceneAuthorityProxy(std::filesystem::path manager_run_root,
                             std::shared_ptr<core::IPlannerSceneProjection> projection);

  [[nodiscard]] core::SceneCommitResultDTO
  Commit(const core::SceneTransactionRequestDTO &request) override;

  [[nodiscard]] core::Result<core::SceneCandidatePreviewDTO>
  PreviewResolvedSceneCandidate(
      const core::ResolvedSceneCandidatePreviewRequestDTO &request,
      const core::CollisionWorldLeaseInventory &predecessor) const override;

  [[nodiscard]] core::RequestOwnerEpoch
  CurrentRequestOwnerEpoch() const override;

  [[nodiscard]] core::ProjectionRevisionDTO
  CurrentProjectionRevision() const override;

  [[nodiscard]] core::ProjectionRevisionDTO
  CurrentProjectionRevision(const std::stop_token stop_token) const;

  [[nodiscard]] std::shared_ptr<const core::LogicalSceneSnapshotDTO>
  LogicalSnapshot() const override;

  [[nodiscard]] std::vector<core::SceneObjectSnapshotDTO>
  GetSceneObjects() const override;

  [[nodiscard]] core::SceneObjectStateDTO GetObjectState(
      const core::SceneResourceId &scene_resource_id) const override;

  [[nodiscard]] core::Result<core::Pose3DTO>
  GetObjectPose(const core::SceneResourceId &scene_resource_id) const override;

private:
  [[nodiscard]] ManagerSceneRead
  Read(const std::stop_token stop_token = {}) const;

  std::filesystem::path manager_run_root_;
  std::shared_ptr<core::IPlannerSceneProjection> projection_;
};

/// Process-local, non-authoritative join from fresh planning admission to the
/// Manager-owned observation policy. It retains no pose or scene truth: every
/// call asks Manager to latch and waits for the native projection to include
/// that revision before the consumer retains its exact planning snapshot.
class ManagerPlanningSceneSynchronizer final
    : public core::IPlanningSceneSynchronizer {
public:
  ManagerPlanningSceneSynchronizer(
      std::filesystem::path manager_run_root,
      core::RetainedBundleId retained_bundle_id,
      core::GraphRevision graph_revision,
      const core::ActiveBundlePublicationGeneration publication_generation,
      std::shared_ptr<core::ICollisionWorldSnapshotPublisher>
          collision_publisher);

  [[nodiscard]] core::Result<core::ProjectionRevisionDTO>
  SynchronizeBeforePlanning() override;

private:
  [[nodiscard]] core::Result<core::ProjectionRevisionDTO>
  Failure(std::string code, std::string message) const;

  [[nodiscard]] CanonicalValue RuntimeIdentityValue() const;

  void RequireRuntimeIdentity(const CanonicalValue &value) const;

  std::filesystem::path manager_run_root_;
  core::RetainedBundleId retained_bundle_id_;
  core::GraphRevision graph_revision_;
  core::ActiveBundlePublicationGeneration publication_generation_;
  std::shared_ptr<core::ICollisionWorldSnapshotPublisher> collision_publisher_;
};
} // namespace manyforge::roboplan_backend::host_detail
