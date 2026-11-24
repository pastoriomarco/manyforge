#pragma once

#include <manyforge_core/types/cell_graph.hpp>
#include <manyforge_core/types/component_type.hpp>
#include <manyforge_deployment/component_type_codec.hpp>
#include <manyforge_deployment/program_binding.hpp>

#include <algorithm>
#include <memory>
#include <string_view>
#include <vector>

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

/// A declared frame role's contact bodies that its composed target binds:
/// an optional body the model leaves unbound (OD-352 A1, a passive tool
/// without a tip) is no contact body.
template <typename ComposedTarget>
[[nodiscard]] std::vector<core::CollisionBodyRoleId> bound_contact_bodies(
    const core::ComponentTypeFrameRoleDTO& declared,
    const ComposedTarget& composed) {
  std::vector<core::CollisionBodyRoleId> result;
  for (const auto& role : declared.contact_body_role_ids) {
    if (std::any_of(composed.collision_bodies.begin(),
                    composed.collision_bodies.end(), [&](const auto& body) {
                      return body.collision_body_role_id == role;
                    }))
      result.push_back(role);
  }
  return result;
}

/// The contact bodies of one TCP of a composed target: the declared frame
/// role's, or, for a frame the type does not declare, the Deployment custom
/// TCP's of that model occurrence. Only bodies the composed target binds.
template <typename ComposedTarget>
[[nodiscard]] std::vector<core::CollisionBodyRoleId> tcp_contact_bodies(
    const core::ComponentTypeFrameRoleDTO* declared,
    const core::CellGraphDTO& graph,
    const core::ModelInstanceId& model_instance_id,
    const core::FrameRoleId& frame_role_id, const ComposedTarget& composed) {
  if (declared != nullptr) return bound_contact_bodies(*declared, composed);
  std::vector<core::CollisionBodyRoleId> result;
  for (const auto& model : graph.model_instances) {
    if (model.model_instance_id != model_instance_id) continue;
    for (const auto& custom : model.custom_tcps) {
      if (custom.frame_role_id != frame_role_id) continue;
      for (const auto& role : custom.contact_body_role_ids) {
        if (std::any_of(composed.collision_bodies.begin(),
                        composed.collision_bodies.end(), [&](const auto& body) {
                          return body.collision_body_role_id == role;
                        }))
          result.push_back(role);
      }
    }
  }
  return result;
}

}  // namespace manyforge::deployment::detail
