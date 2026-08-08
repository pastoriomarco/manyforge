#pragma once

#include <manyforge_core/types/error_code.hpp>
#include <manyforge_core/types/execute_request.hpp>
#include <manyforge_core/types/scene_frame_resolution.hpp>
#include <string>

namespace manyforge::core {

class IReferencePoseResolver {
 public:
  virtual ~IReferencePoseResolver() = default;

  /// Capture the complete immutable reference-resolution context once during
  /// admission. Production resolvers must bind every required compiler path to
  /// an immutable state view here; `resolve_reference_pose` may not query live
  /// scene or controller state afterwards.
  virtual Result<ProgramFrameResolutionContextDTO> capture_program_frame_resolution_context(
      ProgramFrameResolutionCaptureDTO) const {
    return Result<ProgramFrameResolutionContextDTO>::Err(
        ErrorDTO{std::string(to_string(ErrorCode::kMissingReference)),
                 "program reference context capture is unavailable",
                 {{"reason", "program_reference_context_capture_unavailable"}}});
  }

  virtual Result<Pose3DTO> resolve_reference_pose(
      const ReferenceDTO& reference, const ProgramFrameResolutionContextDTO& context) const = 0;

  virtual Result<ResolvedSceneFramePoseDTO> resolve_scene_frame(
      const SceneFrameRefDTO&, const SceneFrameResolutionContextDTO&) const {
    return Result<ResolvedSceneFramePoseDTO>::Err(
        ErrorDTO{std::string(to_string(ErrorCode::kMissingReference)),
                 "scene-frame resolution is unavailable",
                 {{"reason", "scene_frame_resolver_not_installed"}}});
  }
};

}  // namespace manyforge::core
