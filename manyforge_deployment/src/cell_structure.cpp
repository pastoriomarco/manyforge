#include "manyforge_deployment/cell_structure.hpp"

#include <algorithm>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace manyforge::deployment {
namespace {

using AssetKey = std::pair<std::string, std::string>;
using ComponentTypeKey = std::pair<std::string, std::string>;

[[noreturn]] void fail(std::string message) {
  throw CellStructureError(std::move(message));
}

[[nodiscard]] AssetKey asset_key(const core::ModelAssetId& id,
                                 const core::ModelRevision& revision) {
  return {id.value(), revision.value()};
}

[[nodiscard]] AssetKey asset_key(const core::ModelInstanceDTO& instance) {
  return asset_key(instance.model_asset_id, instance.model_revision);
}

[[nodiscard]] AssetKey asset_key(const SourceVerifiedModelAsset& asset) {
  return asset_key(asset.value().model_asset_id, asset.value().model_revision);
}

[[nodiscard]] ComponentTypeKey type_key(
    const core::ComponentTypeId& id,
    const core::ComponentTypeRevision& revision) {
  return {id.value(), revision.value()};
}

[[nodiscard]] ComponentTypeKey type_key(
    const core::ControllableComponentDTO& component) {
  return type_key(component.component_type_id,
                  component.component_type_revision);
}

[[nodiscard]] ComponentTypeKey type_key(
    const QualifiedComponentType& type) {
  return type_key(type.value().component_type_id,
                  type.value().component_type_revision);
}

[[nodiscard]] std::string key_label(std::string_view id,
                                    std::string_view revision) {
  return "'" + std::string(id) + "' at " + std::string(revision);
}

template <typename Key>
[[nodiscard]] bool contains_id(const std::set<Key>& selected,
                               std::string_view id) {
  return std::any_of(selected.begin(), selected.end(),
                     [id](const Key& key) { return key.first == id; });
}

[[nodiscard]] std::map<AssetKey, const SourceVerifiedModelAsset*>
build_asset_index(
    const core::CellGraphDTO& graph,
    std::span<const SourceVerifiedModelAsset> qualified_assets) {
  std::set<AssetKey> selected;
  for (const auto& instance : graph.model_instances) {
    selected.insert(asset_key(instance));
  }

  std::map<AssetKey, const SourceVerifiedModelAsset*> result;
  for (const auto& asset : qualified_assets) {
    const auto key = asset_key(asset);
    if (!selected.contains(key)) {
      fail(contains_id(selected, key.first)
               ? "model-source closure contains stale revision for asset " +
                     key_label(key.first, key.second)
               : "model-source closure contains unused extra asset " +
                     key_label(key.first, key.second));
    }
    if (!result.emplace(key, &asset).second) {
      fail("model-source closure contains duplicate exact asset " +
           key_label(key.first, key.second));
    }
  }
  for (const auto& key : selected) {
    if (!result.contains(key)) {
      fail("model-source closure is missing selected asset " +
           key_label(key.first, key.second));
    }
  }
  return result;
}

[[nodiscard]] std::map<ComponentTypeKey, const QualifiedComponentType*>
build_type_index(
    const core::CellGraphDTO& graph,
    std::span<const QualifiedComponentType> qualified_types) {
  std::set<ComponentTypeKey> selected;
  for (const auto& component : graph.controllable_components) {
    selected.insert(type_key(component));
  }

  std::map<ComponentTypeKey, const QualifiedComponentType*> result;
  for (const auto& type : qualified_types) {
    const auto key = type_key(type);
    if (!selected.contains(key)) {
      fail(contains_id(selected, key.first)
               ? "component-type closure contains stale revision for type " +
                     key_label(key.first, key.second)
               : "component-type closure contains unused extra type " +
                     key_label(key.first, key.second));
    }
    if (!result.emplace(key, &type).second) {
      fail("component-type closure contains duplicate exact type " +
           key_label(key.first, key.second));
    }
  }
  for (const auto& key : selected) {
    if (!result.contains(key)) {
      fail("component-type closure is missing selected type " +
           key_label(key.first, key.second));
    }
  }
  return result;
}

template <typename Binding, typename Id, typename IdGetter>
[[nodiscard]] const Binding& require_binding(
    const std::vector<Binding>& bindings, const Id& id, IdGetter get_id,
    std::string_view kind, const core::ModelAssetDTO& asset) {
  const auto found = std::lower_bound(
      bindings.begin(), bindings.end(), id,
      [&get_id](const Binding& binding, const Id& selected_id) {
        return get_id(binding).value() < selected_id.value();
      });
  if (found == bindings.end() || get_id(*found) != id) {
    fail("component source role selects missing " + std::string(kind) +
         " '" + id.value() + "' in model asset '" +
         asset.model_asset_id.value() + "'");
  }
  return *found;
}

struct RoleContext final {
  StructuralModelOccurrence occurrence;
  const SourceVerifiedModelAsset* asset;
};

[[nodiscard]] const RoleContext& require_role(
    const std::map<std::string, RoleContext, std::less<>>& roles,
    const core::ModelRoleId& role_id, std::string_view context) {
  const auto found = roles.find(role_id.value());
  if (found == roles.end()) {
    fail(std::string(context) + " selects undeclared component model role '" +
         role_id.value() + "'");
  }
  return found->second;
}

[[nodiscard]] StructuralFrameResolution resolve_frame(
    const std::map<std::string, RoleContext, std::less<>>& roles,
    const core::ModelRoleId& model_role_id,
    const core::FrameRoleId& frame_role_id, std::string_view context) {
  const auto& role = require_role(roles, model_role_id, context);
  const auto& asset = role.asset->value();
  const auto& binding = require_binding(
      asset.frame_role_bindings, frame_role_id,
      [](const core::ModelFrameRoleBindingDTO& value)
          -> const core::FrameRoleId& { return value.frame_role_id; },
      "frame role", asset);
  return {role.occurrence, frame_role_id, binding.source_link_name};
}

void require_asset_root(
    const std::map<std::string, RoleContext, std::less<>>& roles,
    const core::ComponentTypeModelFrameRefDTO& frame,
    std::string_view context) {
  const auto& role = require_role(roles, frame.model_role_id, context);
  if (frame.frame_role_id != role.asset->value().root_frame_role_id) {
    fail(std::string(context) + " for model role '" +
         frame.model_role_id.value() +
         "' must select the bound asset root frame role");
  }
}

[[nodiscard]] StructuralCommandTargetResolution resolve_target(
    const core::ComponentTypeCommandTargetDTO& target,
    const std::map<std::string, RoleContext, std::less<>>& roles) {
  StructuralCommandTargetResolution result{
      target.command_target_id, {}, {}, {}, {}};

  result.model_groups.reserve(target.model_group_roles.size());
  for (const auto& group : target.model_group_roles) {
    const auto& role =
        require_role(roles, group.model_role_id, "command-target group");
    const auto& asset = role.asset->value();
    const auto& binding = require_binding(
        asset.model_group_role_bindings, group.asset_model_group_role_id,
        [](const core::ModelGroupRoleBindingDTO& value)
            -> const core::ModelGroupRoleId& {
          return value.model_group_role_id;
        },
        "model-group role", asset);
    result.model_groups.push_back(
        {group.model_group_role_id, role.occurrence,
         group.asset_model_group_role_id, binding.source_group_name});
  }

  result.resources.reserve(target.resource_roles.size());
  for (const auto& resource : target.resource_roles) {
    std::visit(
        [&result, &roles](const auto& selected) {
          using T = std::decay_t<decltype(selected)>;
          if constexpr (
              std::is_same_v<T, core::ComponentTypeModelResourceRoleDTO>) {
            const auto& role = require_role(
                roles, selected.model_role_id, "command-target resource");
            const auto& asset = role.asset->value();
            const auto& binding = require_binding(
                asset.resource_role_bindings,
                selected.asset_resource_role_id,
                [](const core::ModelResourceRoleBindingDTO& value)
                    -> const core::ResourceRoleId& {
                  return value.resource_role_id;
                },
                "resource role", asset);
            result.resources.emplace_back(
                StructuralModelResourceResolution{
                    selected.resource_role_id, role.occurrence,
                    selected.asset_resource_role_id,
                    binding.source_joint_names});
          } else {
            result.resources.emplace_back(
                StructuralComponentLocalResourceResolution{
                    selected.resource_role_id});
          }
        },
        resource);
  }

  result.frames.reserve(target.frame_roles.size());
  for (const auto& frame : target.frame_roles) {
    result.frames.push_back(
        {frame.frame_role_id,
         resolve_frame(roles, frame.model_role_id,
                       frame.asset_frame_role_id, "command-target frame")});
  }

  result.collision_bodies.reserve(target.collision_body_roles.size());
  for (const auto& body : target.collision_body_roles) {
    const auto& role = require_role(
        roles, body.model_role_id, "command-target collision body");
    const auto& asset = role.asset->value();
    const auto& binding = require_binding(
        asset.collision_body_role_bindings,
        body.asset_collision_body_role_id,
        [](const core::ModelCollisionBodyRoleBindingDTO& value)
            -> const core::CollisionBodyRoleId& {
          return value.collision_body_role_id;
        },
        "collision-body role", asset);
    result.collision_bodies.push_back(
        {body.collision_body_role_id, role.occurrence,
         body.asset_collision_body_role_id, binding.source_link_names});
  }
  return result;
}

[[nodiscard]] StructuralComponentResolution resolve_component(
    const core::ControllableComponentDTO& component,
    const QualifiedComponentType& qualified_type,
    const std::map<std::string, const core::ModelInstanceDTO*, std::less<>>&
        instances,
    const std::map<AssetKey, const SourceVerifiedModelAsset*>& assets) {
  const auto& type = qualified_type.value();
  std::set<std::string, std::less<>> declared_roles;
  for (const auto& role : type.model_roles) {
    declared_roles.insert(role.model_role_id.value());
  }

  std::map<std::string, const core::ModelInstanceDTO*, std::less<>>
      bound_instances;
  for (const auto& binding : component.model_role_bindings) {
    if (!declared_roles.contains(binding.model_role_id.value())) {
      fail("component '" + component.controllable_component_id.value() +
           "' binds undeclared model role '" +
           binding.model_role_id.value() + "'");
    }
    const auto instance = instances.find(binding.model_instance_id.value());
    if (instance == instances.end()) {
      fail("component model role references missing model instance '" +
           binding.model_instance_id.value() + "'");
    }
    if (!bound_instances
             .emplace(binding.model_role_id.value(), instance->second)
             .second) {
      fail("component '" + component.controllable_component_id.value() +
           "' binds model role '" + binding.model_role_id.value() +
           "' more than once");
    }
  }
  if (bound_instances.size() != declared_roles.size()) {
    const auto missing = std::find_if(
        declared_roles.begin(), declared_roles.end(),
        [&bound_instances](const std::string& role) {
          return !bound_instances.contains(role);
        });
    fail("component '" + component.controllable_component_id.value() +
         "' is missing exactly-one binding for model role '" +
         (missing == declared_roles.end() ? std::string("<unknown>")
                                          : *missing) +
         "'");
  }

  std::map<std::string, RoleContext, std::less<>> roles;
  std::vector<StructuralModelOccurrence> model_roles;
  model_roles.reserve(type.model_roles.size());
  for (const auto& declared : type.model_roles) {
    const auto* instance = bound_instances.at(declared.model_role_id.value());
    const auto asset = assets.find(asset_key(*instance));
    if (asset == assets.end()) {
      fail("internal structural resolution lost selected model asset");
    }
    StructuralModelOccurrence occurrence{
        declared.model_role_id, instance->model_instance_id,
        instance->model_asset_id, instance->model_revision};
    model_roles.push_back(occurrence);
    roles.emplace(declared.model_role_id.value(),
                  RoleContext{std::move(occurrence), asset->second});
  }

  std::optional<StructuralFrameResolution> component_root;
  if (type.component_root.has_value()) {
    require_asset_root(roles, *type.component_root, "component root");
    component_root =
        resolve_frame(roles, type.component_root->model_role_id,
                      type.component_root->frame_role_id, "component root");
  }

  std::vector<StructuralModelMountResolution> mounts;
  mounts.reserve(type.model_mounts.size());
  for (const auto& mount : type.model_mounts) {
    require_asset_root(roles, mount.child, "model-mount child");
    mounts.push_back(
        {resolve_frame(roles, mount.parent.model_role_id,
                       mount.parent.frame_role_id, "model-mount parent"),
         resolve_frame(roles, mount.child.model_role_id,
                       mount.child.frame_role_id, "model-mount child"),
         mount.child_frame_pose_in_parent_frame});
  }

  std::vector<StructuralCommandTargetResolution> targets;
  targets.reserve(type.command_targets.size());
  for (const auto& target : type.command_targets) {
    targets.push_back(resolve_target(target, roles));
  }

  std::vector<StructuralAttachmentInterfaceResolution> interfaces;
  interfaces.reserve(type.attachment_interfaces.size());
  for (const auto& interface : type.attachment_interfaces) {
    interfaces.push_back(
        {interface.attachment_interface_id,
         resolve_frame(roles, interface.model_role_id,
                       interface.frame_role_id, "attachment interface"),
         interface.accepts_tags, interface.max_attachments});
  }

  return {
      component.controllable_component_id,
      component.component_type_id,
      component.component_type_revision,
      type.tags,
      std::move(model_roles),
      std::move(component_root),
      std::move(mounts),
      std::move(targets),
      std::move(interfaces),
  };
}

[[nodiscard]] const StructuralAttachmentInterfaceResolution&
require_attachment_interface(
    const StructuralComponentResolution& component,
    const core::AttachmentInterfaceId& interface_id) {
  const auto found = std::lower_bound(
      component.attachment_interfaces.begin(),
      component.attachment_interfaces.end(), interface_id,
      [](const StructuralAttachmentInterfaceResolution& value,
         const core::AttachmentInterfaceId& selected_id) {
        return value.attachment_interface_id.value() < selected_id.value();
      });
  if (found == component.attachment_interfaces.end() ||
      found->attachment_interface_id != interface_id) {
    fail("attachment endpoint selects missing interface '" +
         interface_id.value() + "' on component '" +
         component.controllable_component_id.value() + "'");
  }
  return *found;
}

[[nodiscard]] bool tags_intersect(const std::vector<std::string>& accepted,
                                  const std::vector<std::string>& offered) {
  auto accepted_it = accepted.begin();
  auto offered_it = offered.begin();
  while (accepted_it != accepted.end() && offered_it != offered.end()) {
    if (*accepted_it == *offered_it) {
      return true;
    }
    if (*accepted_it < *offered_it) {
      ++accepted_it;
    } else {
      ++offered_it;
    }
  }
  return false;
}

}  // namespace

StructurallyQualifiedCellGraph qualify_cell_structure(
    const GraphRevisionVerifiedCellGraph& verified_graph,
    std::span<const SourceVerifiedModelAsset> qualified_assets,
    std::span<const QualifiedComponentType> qualified_types) {
  const auto& graph = verified_graph.graph();
  const auto asset_index = build_asset_index(graph, qualified_assets);
  const auto type_index = build_type_index(graph, qualified_types);

  std::map<AssetKey, std::uint64_t> asset_occurrences;
  std::map<std::string, const core::ModelInstanceDTO*, std::less<>>
      instance_index;
  for (const auto& instance : graph.model_instances) {
    instance_index.emplace(instance.model_instance_id.value(), &instance);
    const auto key = asset_key(instance);
    auto& count = asset_occurrences[key];
    ++count;
    const auto* asset = asset_index.at(key);
    if (count > 1U &&
        asset->value().instance_cardinality ==
            core::ModelAssetInstanceCardinality::kOnePerCell) {
      fail("model asset " + key_label(key.first, key.second) +
           " permits only one occurrence per cell");
    }
  }

  std::vector<StructuralComponentResolution> components;
  components.reserve(graph.controllable_components.size());
  for (const auto& component : graph.controllable_components) {
    components.push_back(resolve_component(
        component, *type_index.at(type_key(component)), instance_index,
        asset_index));
  }

  std::map<std::string, const StructuralComponentResolution*, std::less<>>
      component_index;
  for (const auto& component : components) {
    component_index.emplace(component.controllable_component_id.value(),
                            &component);
  }

  struct ModelOwner final {
    const StructuralComponentResolution* component;
    const StructuralModelOccurrence* role;
  };
  std::map<std::string, ModelOwner, std::less<>> model_owners;
  for (const auto& component : components) {
    for (const auto& role : component.model_roles) {
      model_owners.emplace(role.model_instance_id.value(),
                           ModelOwner{&component, &role});
    }
  }
  std::set<std::string, std::less<>> attached_components;
  for (const auto& attachment : graph.component_attachments) {
    attached_components.insert(
        attachment.child.controllable_component_id.value());
  }

  std::vector<StructuralModelInstanceResolution> model_instances;
  model_instances.reserve(graph.model_instances.size());
  for (const auto& instance : graph.model_instances) {
    const auto* asset = asset_index.at(asset_key(instance));
    const auto owner = model_owners.find(instance.model_instance_id.value());
    if (owner == model_owners.end()) {
      model_instances.push_back(
          {instance.model_instance_id, instance.model_asset_id,
           instance.model_revision, asset->parsed_ir().root_link_name,
           std::nullopt, std::nullopt,
           StructuralPlacementSource::kDirectModelPoseInCell});
      continue;
    }

    const auto* component = owner->second.component;
    const bool is_component_root =
        component->component_root.has_value() &&
        component->component_root->model.model_instance_id ==
            instance.model_instance_id;
    const auto placement_source =
        !is_component_root
            ? StructuralPlacementSource::kDerivedFromComponentModelMount
            : (attached_components.contains(
                   component->controllable_component_id.value())
                   ? StructuralPlacementSource::
                         kDerivedFromComponentAttachment
                   : StructuralPlacementSource::kDirectComponentPoseInCell);
    model_instances.push_back(
        {instance.model_instance_id, instance.model_asset_id,
         instance.model_revision, asset->parsed_ir().root_link_name,
         component->controllable_component_id,
         owner->second.role->model_role_id, placement_source});
  }

  using EndpointKey = std::pair<std::string, std::string>;
  std::map<EndpointKey, std::uint64_t> endpoint_use_count;
  std::vector<StructuralAttachmentResolution> attachments;
  attachments.reserve(graph.component_attachments.size());
  for (const auto& attachment : graph.component_attachments) {
    const auto parent_component = component_index.find(
        attachment.parent.controllable_component_id.value());
    const auto child_component = component_index.find(
        attachment.child.controllable_component_id.value());
    if (parent_component == component_index.end() ||
        child_component == component_index.end()) {
      fail("attachment endpoint references a missing structurally resolved "
           "component");
    }
    const auto& parent_interface = require_attachment_interface(
        *parent_component->second,
        attachment.parent.attachment_interface_id);
    const auto& child_interface = require_attachment_interface(
        *child_component->second,
        attachment.child.attachment_interface_id);
    if (!child_component->second->component_root.has_value() ||
        child_interface.frame != *child_component->second->component_root) {
      fail("attachment child interface '" +
           child_interface.attachment_interface_id.value() +
           "' must resolve to the child component root");
    }

    if (!tags_intersect(parent_interface.accepts_tags,
                        child_component->second->component_tags)) {
      fail("attachment parent interface '" +
           parent_interface.attachment_interface_id.value() +
           "' does not accept any tag offered by child component '" +
           child_component->second->controllable_component_id.value() + "'");
    }
    if (!tags_intersect(child_interface.accepts_tags,
                        parent_component->second->component_tags)) {
      fail("attachment child interface '" +
           child_interface.attachment_interface_id.value() +
           "' does not accept any tag offered by parent component '" +
           parent_component->second->controllable_component_id.value() + "'");
    }

    const EndpointKey parent_key{
        parent_component->first,
        parent_interface.attachment_interface_id.value()};
    const EndpointKey child_key{
        child_component->first,
        child_interface.attachment_interface_id.value()};
    if (++endpoint_use_count[parent_key] > parent_interface.max_attachments) {
      fail("attachment parent interface '" + parent_key.second +
           "' exceeds its declared capacity");
    }
    if (++endpoint_use_count[child_key] > child_interface.max_attachments) {
      fail("attachment child interface '" + child_key.second +
           "' exceeds its declared capacity");
    }

    attachments.push_back(
        {attachment.component_attachment_id,
         {attachment.parent.controllable_component_id,
          attachment.parent.attachment_interface_id,
          parent_interface.frame},
         {attachment.child.controllable_component_id,
          attachment.child.attachment_interface_id, child_interface.frame},
         attachment.child_interface_pose_in_parent_interface});
  }

  std::vector<SourceVerifiedModelAsset> owned_assets(
      qualified_assets.begin(), qualified_assets.end());
  std::sort(owned_assets.begin(), owned_assets.end(),
            [](const SourceVerifiedModelAsset& left,
               const SourceVerifiedModelAsset& right) {
              return asset_key(left) < asset_key(right);
            });
  std::vector<QualifiedComponentType> owned_types(
      qualified_types.begin(), qualified_types.end());
  std::sort(owned_types.begin(), owned_types.end(),
            [](const QualifiedComponentType& left,
               const QualifiedComponentType& right) {
              return type_key(left) < type_key(right);
            });

  return StructurallyQualifiedCellGraph(
      verified_graph, std::move(owned_assets), std::move(owned_types),
      std::move(model_instances), std::move(components),
      std::move(attachments));
}

}  // namespace manyforge::deployment
