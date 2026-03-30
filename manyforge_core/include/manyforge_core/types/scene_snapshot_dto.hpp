#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace manyforge::core {

/// Pose in 3D space (position + quaternion orientation).
struct Pose {
  double x = 0.0;
  double y = 0.0;
  double z = 0.0;
  double qx = 0.0;
  double qy = 0.0;
  double qz = 0.0;
  double qw = 1.0;
};

/// A collision object in the scene.
struct CollisionObjectDTO {
  std::string id;
  std::string shape_type;  // "box", "cylinder", "sphere", "mesh"
  std::vector<double> dimensions;
  Pose pose;
  std::string attached_to_link;  // empty if not attached
};

/// Robot joint state at a point in time.
struct JointStateDTO {
  std::vector<std::string> names;
  std::vector<double> positions;
  std::vector<double> velocities;
};

/// A snapshot of the planning scene at a point in time.
/// ADR-0007: deterministic snapshot for planning inputs.
/// ADR-0008: version/hash used for cache keying and invalidation.
struct SceneSnapshotDTO {
  /// Monotonically increasing version number.
  uint64_t version = 0;

  /// Hash of scene contents for fast equality checks.
  uint64_t content_hash = 0;

  /// Collision objects present in the scene.
  std::vector<CollisionObjectDTO> objects;

  /// Current robot joint state.
  JointStateDTO robot_state;
};

}  // namespace manyforge::core
