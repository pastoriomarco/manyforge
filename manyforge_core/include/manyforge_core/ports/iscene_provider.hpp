#pragma once

#include <functional>

#include <manyforge_core/types/error_code.hpp>
#include <manyforge_core/types/scene_snapshot_dto.hpp>

namespace manyforge::core {

/// Callback type for scene update notifications.
using SceneUpdateCallback = std::function<void(const SceneSnapshotDTO&)>;

/// Abstract scene provider interface.
/// ADR-0007: explicit scene ownership; snapshot-based for deterministic planning.
/// ADR-0002: port interface in core; adapters wrap MoveIt PlanningScene.
class ISceneProvider {
 public:
  virtual ~ISceneProvider() = default;

  /// Get a consistent snapshot of the current scene.
  /// The snapshot includes version/hash for cache keying (ADR-0008).
  virtual Result<SceneSnapshotDTO> get_snapshot() = 0;

  /// Subscribe to scene update notifications.
  /// The callback is invoked whenever the scene changes.
  /// Returns a subscription ID that could be used for future unsubscribe.
  virtual uint64_t subscribe_updates(SceneUpdateCallback callback) = 0;
};

}  // namespace manyforge::core
