#pragma once

namespace manyforge::core {

/// Collision severity levels (legacy type for collision_policy)
enum class CollisionSeverity {
  kWarning,
  kSoftStop,
  kHardStop,
};

/// Collision event (legacy type for collision_policy)
struct CollisionEvent {
  CollisionSeverity severity = CollisionSeverity::kHardStop;
};

}  // namespace manyforge::core
