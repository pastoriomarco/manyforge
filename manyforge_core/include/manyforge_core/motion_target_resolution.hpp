#pragma once

#include <manyforge_core/ports/ireference_pose_resolver.hpp>
#include <manyforge_core/types/execute_request.hpp>
#include <manyforge_core/types/robot_state.hpp>

namespace manyforge::core {

/// Resolve one portable motion target against an immutable scene binding.
/// This is the only policy implementation shared by kernel admission and
/// non-dispatching preview/plan surfaces.
Result<MotionSpecDTO> resolve_motion_target(
    const ExecuteRequestDTO& request,
    const SceneRevisionBindingDTO& scene_binding,
    const RobotStateDTO* observed_start_state,
    const IReferencePoseResolver& reference_resolver);

}  // namespace manyforge::core
