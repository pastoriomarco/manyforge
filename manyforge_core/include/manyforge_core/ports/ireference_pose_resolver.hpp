#pragma once

#include <string>

#include <manyforge_core/types/error_code.hpp>
#include <manyforge_core/types/motion_spec.hpp>
#include <manyforge_core/types/projection_common.hpp>
#include <manyforge_core/types/scene_frame_resolution.hpp>

namespace manyforge::core {

class IReferencePoseResolver {
 public:
  virtual ~IReferencePoseResolver() = default;

  virtual Result<Pose3DTO> resolve_reference_pose(
      const ReferenceDTO& reference, const std::string& base_frame,
      const ProjectionLeaseBindingDTO& projection_binding) const = 0;

  virtual Result<ResolvedSceneFramePoseDTO> resolve_scene_frame(
      const SceneFrameRefDTO&,
      const SceneFrameResolutionContextDTO&) const {
    return Result<ResolvedSceneFramePoseDTO>::Err(
        ErrorDTO{std::string(to_string(ErrorCode::kMissingReference)),
                 "scene-frame resolution is unavailable",
                 {{"reason", "scene_frame_resolver_not_installed"}}});
  }
};

}  // namespace manyforge::core
