#pragma once

#include <manyforge_core/types/component_type.hpp>
#include <manyforge_deployment/component_type_codec.hpp>
#include <manyforge_deployment/program_binding.hpp>

#include <algorithm>
#include <memory>
#include <string_view>

namespace manyforge::deployment::detail {

[[nodiscard]] CanonicalValue program_binding_projection(
    const core::ProgramBindingDTO& value);

/// Sole in-process producer used by the binding compiler after exact role
/// resolution. The supplied ID is ignored and recomputed from the complete
/// normalized value.
class ProgramBindingConstructionAuthority final {
 public:
  [[nodiscard]] static std::shared_ptr<const ProgramBinding> seal(
      core::ProgramBindingDTO value);

  [[nodiscard]] static std::shared_ptr<const ProgramBinding> reload_intrinsic(
      std::string_view canonical_bytes);
};

/// The declared frame role a target frame role binds as; null when the type
/// does not declare it (a custom TCP).
[[nodiscard]] inline const core::ComponentTypeFrameRoleDTO*
declared_target_frame_role(const core::ComponentTypeCommandTargetDTO& target,
                           const core::FrameRoleId& role) {
  const auto found = std::find_if(
      target.frame_roles.begin(), target.frame_roles.end(),
      [&](const auto& value) { return value.frame_role_id == role; });
  return found == target.frame_roles.end() ? nullptr : &*found;
}

}  // namespace manyforge::deployment::detail
