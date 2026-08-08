#pragma once

#include <cstddef>

#include <manyforge_core/types/collision.hpp>
#include <manyforge_core/types/coordinate_state.hpp>
#include <manyforge_core/types/geometry.hpp>
#include <manyforge_core/types/projection_common.hpp>
#include <manyforge_core/types/robot_state.hpp>
#include <manyforge_core/types/trajectory_dto.hpp>

namespace manyforge::core {

/// Immutable result of one non-dispatching plan/validate pass.
///
/// This is an owner-private C++ value, not a public request surface. The
/// trajectory and terminal state are retained so deterministic multi-leg
/// candidate search can chain the exact output of one leg into the next
/// without sampling mutable robot state between legs.
struct MotionPreviewResultDTO final {
  TrajectoryDTO trajectory;
  RobotStateDTO start_state;
  RobotStateDTO final_state;
  /// Exact typed terminal state for the selected execution binding. This is
  /// predicted candidate evidence, not an observed-state publication.
  ControlledStateDTO final_controlled_state;
  Pose3DTO final_tcp_pose;
  ProjectionLeaseBindingDTO projection_binding;
  CollisionWorldRevision collision_world_revision;
  std::size_t dynamically_discovered_start_contact_count{};
};

}  // namespace manyforge::core
