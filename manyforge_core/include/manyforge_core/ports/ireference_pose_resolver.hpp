#pragma once

#include <string>

#include <manyforge_core/types/error_code.hpp>
#include <manyforge_core/types/motion_spec.hpp>
#include <manyforge_core/types/scene_authority.hpp>

namespace manyforge::core {

class IReferencePoseResolver {
 public:
  virtual ~IReferencePoseResolver() = default;

  virtual Result<Pose3DTO> resolve_reference_pose(
      const ReferenceDTO& reference, const std::string& base_frame,
      const SceneRevisionBindingDTO& scene_binding) const = 0;
};

}  // namespace manyforge::core
