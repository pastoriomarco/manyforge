#pragma once

#include <functional>
#include <optional>

#include <manyforge_core/ports/icontroller.hpp>
#include <manyforge_core/types/collision_event.hpp>
#include <manyforge_core/types/error_code.hpp>
#include <manyforge_core/types/scene_snapshot_dto.hpp>
#include <manyforge_core/types/trajectory_dto.hpp>

namespace manyforge::core {

/// Abstract collision monitor interface.
/// ADR-0009: collision monitoring is first-class and policy-driven.
/// ADR-0002: port interface in core; adapters wrap collision checking backends.
class ICollisionMonitor {
 public:
  virtual ~ICollisionMonitor() = default;

  /// Check a trajectory for collisions before execution begins.
  /// Returns a CollisionEvent if a collision is detected, empty optional if clear.
  virtual Result<std::optional<CollisionEvent>> pre_execute_check(
      const TrajectoryDTO& trajectory, const SceneSnapshotDTO& scene) = 0;

  /// Check for collisions during execution given current feedback and scene.
  /// Called periodically by the Kernel during execution.
  /// Returns a CollisionEvent if detected, empty optional if clear.
  virtual Result<std::optional<CollisionEvent>> during_execute_monitor(
      const ExecutionFeedback& feedback, const SceneSnapshotDTO& scene) = 0;
};

}  // namespace manyforge::core
