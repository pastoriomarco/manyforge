#pragma once

#include <manyforge_core/types/scene_transaction.hpp>
#include <memory>
#include <vector>

namespace manyforge::core {

/// Sole production logical-scene edit and query authority.
class ISceneAuthority {
 public:
  virtual ~ISceneAuthority() = default;

  [[nodiscard]] virtual SceneCommitResultDTO Commit(const SceneTransactionRequestDTO& request) = 0;
  [[nodiscard]] virtual RequestOwnerEpoch CurrentRequestOwnerEpoch() const = 0;
  [[nodiscard]] virtual ProjectionRevisionDTO CurrentProjectionRevision() const = 0;
  [[nodiscard]] virtual std::shared_ptr<const LogicalSceneSnapshotDTO> LogicalSnapshot() const = 0;
  [[nodiscard]] virtual std::vector<SceneObjectSnapshotDTO> GetSceneObjects() const = 0;
  [[nodiscard]] virtual SceneObjectStateDTO GetObjectState(
      const SceneResourceId& scene_resource_id) const = 0;
  [[nodiscard]] virtual Result<Pose3DTO> GetObjectPose(
      const SceneResourceId& scene_resource_id) const = 0;
};

}  // namespace manyforge::core
