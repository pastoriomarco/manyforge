#include "manyforge_deployment/component_type_codec.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "catalog_record_referent_internal.hpp"
#include "content_identity_internal.hpp"
#include "compiler_semantic_limits_internal.hpp"

namespace manyforge::deployment {
namespace {

using CapabilityKey = std::pair<std::string, std::string>;
using ModelSourceKey = std::pair<std::string, std::string>;
using CapabilityIndex =
    std::map<CapabilityKey, const QualifiedCapabilityDefinition*>;

[[noreturn]] void fail(std::string message) {
  throw ComponentTypeCodecError(std::move(message));
}

[[nodiscard]] const CanonicalValue& require_member(
    const CanonicalValue& object, std::string_view name) {
  const auto* member = object.find_object_member(name);
  if (member == nullptr) {
    fail("component-type normalized value is missing " + std::string(name));
  }
  return *member;
}

[[nodiscard]] const std::string& require_string(
    const CanonicalValue& value, std::string_view name) {
  const auto* text = std::get_if<std::string>(&value.storage());
  if (text == nullptr) {
    fail("component-type normalized field is not a string: " +
         std::string(name));
  }
  return *text;
}

[[nodiscard]] const CanonicalValue::Array& require_array(
    const CanonicalValue& value, std::string_view name) {
  const auto* array = std::get_if<CanonicalValue::Array>(&value.storage());
  if (array == nullptr) {
    fail("component-type normalized field is not an array: " +
         std::string(name));
  }
  return *array;
}

[[nodiscard]] double require_number(const CanonicalValue& value,
                                    std::string_view name) {
  const auto* number = std::get_if<double>(&value.storage());
  if (number == nullptr || !std::isfinite(*number)) {
    fail("component-type normalized field is not a finite number: " +
         std::string(name));
  }
  return *number;
}

[[nodiscard]] std::uint64_t require_positive_integer(
    const CanonicalValue& value, std::string_view name) {
  const auto number = require_number(value, name);
  if (std::floor(number) != number || number < 1.0 ||
      number >
          static_cast<double>(
              core::kMaxComponentTypeAttachmentsPerInterface)) {
    fail("component-type normalized max_attachments is outside the v1 "
         "range");
  }
  return static_cast<std::uint64_t>(number);
}

[[nodiscard]] std::vector<std::string> decode_strings(
    const CanonicalValue& value, std::string_view name) {
  const auto& source = require_array(value, name);
  std::vector<std::string> result;
  result.reserve(source.size());
  for (const auto& item : source) {
    result.push_back(require_string(item, name));
  }
  return result;
}

[[nodiscard]] core::ComponentTypeModelFrameRefDTO decode_frame_ref(
    const CanonicalValue& value, std::string_view name) {
  return {
      core::ModelRoleId(require_string(
          require_member(value, "model_role_id"),
          std::string(name) + ".model_role_id")),
      core::FrameRoleId(require_string(
          require_member(value, "frame_role_id"),
          std::string(name) + ".frame_role_id")),
  };
}

[[nodiscard]] core::PlacementPoseDTO decode_placement_pose(
    const CanonicalValue& value) {
  const auto& position =
      require_array(require_member(value, "position_m"), "position_m");
  const auto& orientation = require_array(
      require_member(value, "orientation_xyzw"), "orientation_xyzw");
  if (position.size() != 3U || orientation.size() != 4U) {
    fail("component-type normalized placement pose has invalid dimensions");
  }

  core::PlacementPoseDTO result;
  for (std::size_t index = 0U; index < result.position_m.size(); ++index) {
    result.position_m[index] = require_number(position[index], "position_m");
  }
  double norm_squared = 0.0;
  for (std::size_t index = 0U; index < result.orientation_xyzw.size();
       ++index) {
    result.orientation_xyzw[index] =
        require_number(orientation[index], "orientation_xyzw");
    norm_squared +=
        result.orientation_xyzw[index] * result.orientation_xyzw[index];
  }
  if (!std::isfinite(norm_squared) ||
      std::abs(norm_squared - 1.0) >
          detail::kQuaternionNormSquaredTolerance) {
    fail("component-type placement quaternion norm squared must be within "
         "1e-6 of one");
  }
  return result;
}

[[nodiscard]] std::vector<core::ComponentTypeModelRoleDTO>
decode_model_roles(const CanonicalValue& value) {
  const auto& source = require_array(value, "model_roles");
  std::vector<core::ComponentTypeModelRoleDTO> result;
  result.reserve(source.size());
  for (const auto& item : source) {
    const auto& cardinality =
        require_string(require_member(item, "cardinality"), "cardinality");
    if (cardinality != "exactly_one") {
      fail("component-type model role has unknown cardinality");
    }
    result.push_back({
        core::ModelRoleId(require_string(
            require_member(item, "model_role_id"), "model_role_id")),
        core::ComponentTypeModelRoleCardinality::kExactlyOne,
    });
  }
  return result;
}

[[nodiscard]] std::vector<core::ComponentTypeModelMountDTO>
decode_model_mounts(const CanonicalValue& value) {
  const auto& source = require_array(value, "model_mounts");
  std::vector<core::ComponentTypeModelMountDTO> result;
  result.reserve(source.size());
  for (const auto& item : source) {
    result.push_back({
        decode_frame_ref(require_member(item, "parent"), "parent"),
        decode_frame_ref(require_member(item, "child"), "child"),
        decode_placement_pose(
            require_member(item, "child_frame_pose_in_parent_frame")),
    });
  }
  return result;
}

[[nodiscard]] std::vector<core::ComponentTypeModelGroupRoleDTO>
decode_group_roles(const CanonicalValue& value) {
  const auto& source = require_array(value, "model_group_roles");
  std::vector<core::ComponentTypeModelGroupRoleDTO> result;
  result.reserve(source.size());
  for (const auto& item : source) {
    result.push_back({
        core::ModelGroupRoleId(require_string(
            require_member(item, "model_group_role_id"),
            "model_group_role_id")),
        core::ModelRoleId(require_string(
            require_member(item, "model_role_id"), "model_role_id")),
        core::ModelGroupRoleId(require_string(
            require_member(item, "asset_model_group_role_id"),
            "asset_model_group_role_id")),
    });
  }
  return result;
}

[[nodiscard]] core::ComponentTypeResourceSourceKind decode_source_kind(
    const CanonicalValue& value) {
  const auto& source_kind = require_string(value, "source_kind");
  if (source_kind == "model_role") {
    return core::ComponentTypeResourceSourceKind::kModelRole;
  }
  if (source_kind == "component_local") {
    return core::ComponentTypeResourceSourceKind::kComponentLocal;
  }
  fail("component-type resource role has unknown source_kind");
}

[[nodiscard]] std::optional<core::CoordinateKind>
decode_optional_coordinate_kind(const CanonicalValue& value) {
  const auto* member = value.find_object_member("coordinate_kind");
  if (member == nullptr) {
    return std::nullopt;
  }
  const auto& kind = require_string(*member, "coordinate_kind");
  if (kind == "angular") {
    return core::CoordinateKind::kAngular;
  }
  if (kind == "linear") {
    return core::CoordinateKind::kLinear;
  }
  fail("component-type local resource has unknown coordinate_kind");
}

[[nodiscard]] std::vector<core::ComponentTypeResourceRoleDTO>
decode_resource_roles(const CanonicalValue& value) {
  const auto& source = require_array(value, "resource_roles");
  std::vector<core::ComponentTypeResourceRoleDTO> result;
  result.reserve(source.size());
  for (const auto& item : source) {
    const auto kind =
        decode_source_kind(require_member(item, "source_kind"));
    if (kind == core::ComponentTypeResourceSourceKind::kModelRole) {
      result.emplace_back(core::ComponentTypeModelResourceRoleDTO{
          core::ResourceRoleId(require_string(
              require_member(item, "resource_role_id"),
              "resource_role_id")),
          core::ModelRoleId(require_string(
              require_member(item, "model_role_id"), "model_role_id")),
          core::ResourceRoleId(require_string(
              require_member(item, "asset_resource_role_id"),
              "asset_resource_role_id")),
      });
    } else {
      result.emplace_back(core::ComponentTypeLocalResourceRoleDTO{
          core::ResourceRoleId(require_string(
              require_member(item, "resource_role_id"),
              "resource_role_id")),
          decode_optional_coordinate_kind(item),
      });
    }
  }
  return result;
}

[[nodiscard]] std::vector<core::ComponentTypeFrameRoleDTO>
decode_frame_roles(const CanonicalValue& value) {
  const auto& source = require_array(value, "frame_roles");
  std::vector<core::ComponentTypeFrameRoleDTO> result;
  result.reserve(source.size());
  for (const auto& item : source) {
    std::vector<core::CollisionBodyRoleId> contact_body_role_ids;
    if (const auto* contact_bodies =
            item.find_object_member("contact_body_role_ids")) {
      const auto& values =
          require_array(*contact_bodies, "contact_body_role_ids");
      if (values.size() > core::kMaxComponentTypeTargetRoleAliases) {
        fail("component-type frame contact-body role list exceeds fixed "
             "bound");
      }
      contact_body_role_ids.reserve(values.size());
      for (const auto& value : values) {
        contact_body_role_ids.emplace_back(require_string(
            value, "contact_body_role_ids"));
      }
    }
    bool tcp = false;
    if (const auto* tagged = item.find_object_member("tcp")) {
      const auto* flag = std::get_if<bool>(&tagged->storage());
      if (flag == nullptr) {
        fail("component-type normalized field is not a boolean: tcp");
      }
      tcp = *flag;
    }
    result.push_back({
        core::FrameRoleId(require_string(
            require_member(item, "frame_role_id"), "frame_role_id")),
        core::ModelRoleId(require_string(
            require_member(item, "model_role_id"), "model_role_id")),
        core::FrameRoleId(require_string(
            require_member(item, "asset_frame_role_id"),
            "asset_frame_role_id")),
        std::move(contact_body_role_ids),
        tcp,
    });
  }
  return result;
}

[[nodiscard]] std::vector<core::ComponentTypeCollisionBodyRoleDTO>
decode_collision_body_roles(const CanonicalValue& value) {
  const auto& source = require_array(value, "collision_body_roles");
  std::vector<core::ComponentTypeCollisionBodyRoleDTO> result;
  result.reserve(source.size());
  for (const auto& item : source) {
    result.push_back({
        core::CollisionBodyRoleId(require_string(
            require_member(item, "collision_body_role_id"),
            "collision_body_role_id")),
        core::ModelRoleId(require_string(
            require_member(item, "model_role_id"), "model_role_id")),
        core::CollisionBodyRoleId(require_string(
            require_member(item, "asset_collision_body_role_id"),
            "asset_collision_body_role_id")),
    });
  }
  return result;
}

[[nodiscard]] std::vector<core::ComponentTypeCapabilityDTO>
decode_capabilities(const CanonicalValue& value) {
  const auto& source = require_array(value, "capabilities");
  std::vector<core::ComponentTypeCapabilityDTO> result;
  result.reserve(source.size());
  for (const auto& item : source) {
    result.push_back({
        core::CapabilityId(require_string(
            require_member(item, "capability_id"), "capability_id")),
        core::CapabilityDefinitionRevision(require_string(
            require_member(item, "capability_definition_revision"),
            "capability_definition_revision")),
    });
  }
  return result;
}

[[nodiscard]] std::vector<core::ComponentTypeCommandTargetDTO>
decode_command_targets(const CanonicalValue& value) {
  const auto& source = require_array(value, "command_targets");
  std::vector<core::ComponentTypeCommandTargetDTO> result;
  result.reserve(source.size());
  for (const auto& item : source) {
    std::optional<core::FrameRoleId> default_tcp;
    if (const auto* selected =
            item.find_object_member("default_tcp_frame_role_id")) {
      default_tcp = core::FrameRoleId(
          require_string(*selected, "default_tcp_frame_role_id"));
    }
    result.push_back({
        core::CommandTargetId(require_string(
            require_member(item, "command_target_id"),
            "command_target_id")),
        decode_group_roles(require_member(item, "model_group_roles")),
        decode_resource_roles(require_member(item, "resource_roles")),
        decode_frame_roles(require_member(item, "frame_roles")),
        decode_collision_body_roles(
            require_member(item, "collision_body_roles")),
        decode_capabilities(require_member(item, "capabilities")),
        std::move(default_tcp),
    });
  }
  return result;
}

[[nodiscard]] std::vector<core::ComponentTypeAttachmentInterfaceDTO>
decode_attachment_interfaces(const CanonicalValue& value) {
  const auto& source = require_array(value, "attachment_interfaces");
  std::vector<core::ComponentTypeAttachmentInterfaceDTO> result;
  result.reserve(source.size());
  for (const auto& item : source) {
    result.push_back({
        core::AttachmentInterfaceId(require_string(
            require_member(item, "attachment_interface_id"),
            "attachment_interface_id")),
        core::ModelRoleId(require_string(
            require_member(item, "model_role_id"), "model_role_id")),
        core::FrameRoleId(require_string(
            require_member(item, "frame_role_id"), "frame_role_id")),
        decode_strings(require_member(item, "accepts_tags"),
                       "accepts_tags"),
        require_positive_integer(require_member(item, "max_attachments"),
                                 "max_attachments"),
    });
  }
  return result;
}

[[nodiscard]] core::ComponentTypeDTO decode_normalized(
    const CanonicalValue& normalized) {
  std::optional<core::ComponentTypeModelFrameRefDTO> root;
  if (const auto* component_root =
          normalized.find_object_member("component_root");
      component_root != nullptr) {
    root = decode_frame_ref(*component_root, "component_root");
  }
  return {
      core::ComponentTypeId(require_string(
          require_member(normalized, "component_type_id"),
          "component_type_id")),
      core::ComponentTypeRevision(require_string(
          require_member(normalized, "component_type_revision"),
          "component_type_revision")),
      decode_strings(require_member(normalized, "tags"), "tags"),
      decode_model_roles(require_member(normalized, "model_roles")),
      std::move(root),
      decode_model_mounts(require_member(normalized, "model_mounts")),
      decode_command_targets(require_member(normalized, "command_targets")),
      decode_attachment_interfaces(
          require_member(normalized, "attachment_interfaces")),
  };
}

[[nodiscard]] CapabilityKey capability_key(
    const core::CapabilityId& id,
    const core::CapabilityDefinitionRevision& revision) {
  return {id.value(), revision.value()};
}

[[nodiscard]] CapabilityIndex build_capability_index(
    std::span<const QualifiedCapabilityDefinition> capabilities) {
  if (capabilities.size() >
      core::kMaxComponentTypeCapabilityDependencies) {
    fail("qualified capability dependency view exceeds its fixed bound");
  }
  CapabilityIndex result;
  std::map<std::string, std::string, std::less<>> revisions_by_id;
  for (const auto& capability : capabilities) {
    const auto& value = capability.value();
    const auto [revision, inserted_revision] = revisions_by_id.emplace(
        value.capability_id.value(),
        value.capability_definition_revision.value());
    if (!inserted_revision &&
        revision->second != value.capability_definition_revision.value()) {
      fail("qualified capability dependency view contains more than one "
           "revision of one capability ID");
    }
    if (!result
             .emplace(capability_key(
                          value.capability_id,
                          value.capability_definition_revision),
                      &capability)
             .second) {
      fail("qualified capability dependency view contains a duplicate exact "
           "referent");
    }
  }
  return result;
}

[[nodiscard]] const QualifiedCapabilityDefinition&
require_qualified_capability(const CapabilityIndex& capabilities,
                             const CapabilityKey& key) {
  const auto exact = capabilities.find(key);
  if (exact != capabilities.end()) {
    return *exact->second;
  }
  const bool stale = std::any_of(
      capabilities.begin(), capabilities.end(),
      [&key](const auto& entry) { return entry.first.first == key.first; });
  fail(stale ? "component-type selects a stale capability-definition "
                   "revision"
             : "component-type selects a missing qualified capability "
                   "definition");
}

void require_model_role(const std::set<std::string>& model_roles,
                        const core::ModelRoleId& model_role_id,
                        std::string_view context) {
  if (!model_roles.contains(model_role_id.value())) {
    fail("component-type " + std::string(context) +
         " references a missing model role");
  }
}

void validate_structure(const core::ComponentTypeDTO& type,
                        const std::set<std::string>& model_roles) {
  if (model_roles.empty()) {
    if (type.component_root.has_value() || !type.model_mounts.empty() ||
        !type.attachment_interfaces.empty()) {
      fail("component-type without model roles cannot declare a root, mount, "
           "or attachment interface");
    }
    return;
  }
  if (!type.component_root.has_value()) {
    fail("component-type with model roles requires component_root");
  }
  require_model_role(model_roles, type.component_root->model_role_id,
                     "component_root");
  if (type.model_mounts.size() + 1U != model_roles.size()) {
    fail("component-type model mounts do not have tree cardinality");
  }

  const auto& root = type.component_root->model_role_id.value();
  std::map<std::string, std::vector<std::string>, std::less<>> children;
  for (const auto& role : model_roles) {
    children.emplace(role, std::vector<std::string>{});
  }
  std::set<std::string> child_roles;
  for (const auto& mount : type.model_mounts) {
    require_model_role(model_roles, mount.parent.model_role_id,
                       "mount parent");
    require_model_role(model_roles, mount.child.model_role_id,
                       "mount child");
    const auto& parent = mount.parent.model_role_id.value();
    const auto& child = mount.child.model_role_id.value();
    if (parent == child) {
      fail("component-type mount cannot connect one model role to itself");
    }
    if (child == root) {
      fail("component-type root model role cannot be a mount child");
    }
    if (!child_roles.insert(child).second) {
      fail("component-type non-root model role has more than one parent");
    }
    children.at(parent).push_back(child);
  }
  if (child_roles.size() + 1U != model_roles.size()) {
    fail("component-type every non-root model role must have one parent");
  }

  std::set<std::string> visited;
  std::vector<std::string> pending{root};
  while (!pending.empty()) {
    auto role = std::move(pending.back());
    pending.pop_back();
    if (!visited.insert(role).second) {
      fail("component-type model mount graph contains a cycle");
    }
    for (const auto& child : children.at(role)) {
      pending.push_back(child);
    }
  }
  if (visited.size() != model_roles.size()) {
    fail("component-type model mounts do not form one connected tree");
  }
}

template <typename AliasId, typename AssetId, typename Row,
          typename AliasAccessor, typename ModelAccessor,
          typename AssetAccessor>
void validate_model_aliases(const std::vector<Row>& rows,
                            const std::set<std::string>& model_roles,
                            std::string_view context,
                            AliasAccessor alias_accessor,
                            ModelAccessor model_accessor,
                            AssetAccessor asset_accessor) {
  std::set<std::string> aliases;
  std::set<ModelSourceKey> sources;
  for (const auto& row : rows) {
    const AliasId& alias = alias_accessor(row);
    const auto& model_role = model_accessor(row);
    const AssetId& asset_role = asset_accessor(row);
    if (!aliases.insert(alias.value()).second) {
      fail("component-type " + std::string(context) +
           " contains a duplicate target-local alias");
    }
    require_model_role(model_roles, model_role, context);
    if (!sources
             .emplace(model_role.value(), asset_role.value())
             .second) {
      fail("component-type " + std::string(context) +
           " maps two aliases to the same exact model source");
    }
  }
}

void validate_target(
    const core::ComponentTypeCommandTargetDTO& target,
    const std::set<std::string>& model_roles,
    const CapabilityIndex& qualified_capabilities,
    std::set<CapabilityKey>& selected_capabilities,
    std::map<std::string, std::string, std::less<>>&
        selected_revisions_by_id) {
  validate_model_aliases<core::ModelGroupRoleId, core::ModelGroupRoleId>(
      target.model_group_roles, model_roles, "model_group_roles",
      [](const auto& row) -> const auto& { return row.model_group_role_id; },
      [](const auto& row) -> const auto& { return row.model_role_id; },
      [](const auto& row) -> const auto& {
        return row.asset_model_group_role_id;
      });
  validate_model_aliases<core::FrameRoleId, core::FrameRoleId>(
      target.frame_roles, model_roles, "frame_roles",
      [](const auto& row) -> const auto& { return row.frame_role_id; },
      [](const auto& row) -> const auto& { return row.model_role_id; },
      [](const auto& row) -> const auto& { return row.asset_frame_role_id; });
  validate_model_aliases<core::CollisionBodyRoleId,
                         core::CollisionBodyRoleId>(
      target.collision_body_roles, model_roles, "collision_body_roles",
      [](const auto& row) -> const auto& {
        return row.collision_body_role_id;
      },
      [](const auto& row) -> const auto& { return row.model_role_id; },
      [](const auto& row) -> const auto& {
        return row.asset_collision_body_role_id;
      });

  for (const auto& frame : target.frame_roles) {
    if (frame.contact_body_role_ids.size() >
        core::kMaxComponentTypeTargetRoleAliases) {
      fail("component-type frame contact-body role list exceeds fixed "
           "bound");
    }
    std::set<std::string> associated_roles;
    for (const auto& body_role_id : frame.contact_body_role_ids) {
      if (body_role_id.empty() ||
          !associated_roles.insert(body_role_id.value()).second) {
        fail("component-type frame contact-body roles contain an empty or "
             "duplicate role");
      }
      const auto body = std::find_if(
          target.collision_body_roles.begin(), target.collision_body_roles.end(),
          [&](const auto& candidate) {
            return candidate.collision_body_role_id == body_role_id;
          });
      if (body == target.collision_body_roles.end() ||
          body->model_role_id != frame.model_role_id) {
        fail("component-type frame contact-body role is not exposed by the "
             "same command target and model role");
      }
    }
  }

  std::set<std::string> resource_aliases;
  std::set<ModelSourceKey> resource_sources;
  for (const auto& resource : target.resource_roles) {
    if (!resource_aliases
             .insert(core::resource_role_id(resource).value())
             .second) {
      fail("component-type resource_roles contains a duplicate target-local "
           "alias");
    }
    std::visit(
        [&](const auto& source) {
          using Source = std::decay_t<decltype(source)>;
          if constexpr (std::is_same_v<
                            Source,
                            core::ComponentTypeModelResourceRoleDTO>) {
            require_model_role(model_roles, source.model_role_id,
                               "resource_roles");
            if (!resource_sources
                     .emplace(source.model_role_id.value(),
                              source.asset_resource_role_id.value())
                     .second) {
              fail("component-type resource_roles maps two aliases to the "
                   "same exact model source");
            }
          }
        },
        resource);
  }

  // TCPs: a target that tags any frame role as a TCP names one of them as
  // its default; a motion target (it executes trajectories) also exposes
  // its robot `flange` as a TCP, the point the vendor speed rating refers to.
  const auto tagged = [&target](const core::FrameRoleId& id) {
    return std::any_of(
        target.frame_roles.begin(), target.frame_roles.end(),
        [&id](const auto& frame) { return frame.tcp && frame.frame_role_id == id; });
  };
  const bool has_tcp = std::any_of(
      target.frame_roles.begin(), target.frame_roles.end(),
      [](const auto& frame) { return frame.tcp; });
  if (has_tcp != target.default_tcp_frame_role_id.has_value() ||
      (has_tcp && !tagged(*target.default_tcp_frame_role_id))) {
    fail("default_tcp_unknown: command target '" +
         target.command_target_id.value() +
         "' must name exactly one of its TCP frame roles as default TCP");
  }
  const bool motion_target = std::any_of(
      target.capabilities.begin(), target.capabilities.end(),
      [](const auto& capability) {
        return capability.capability_id.value() ==
               "execution.trajectory.execute";
      });
  if (has_tcp && motion_target && !tagged(core::FrameRoleId("flange"))) {
    fail("robot_flange_tcp_missing: motion target '" +
         target.command_target_id.value() + "' exposes no flange TCP");
  }
  // O5: a passive tool's frame-only target (frame and collision roles only)
  // selects no capability.
  if (target.capabilities.empty() &&
      (!target.model_group_roles.empty() || !target.resource_roles.empty())) {
    fail("component-type target without capabilities may expose only frame "
         "and collision-body roles");
  }

  bool requires_control_binding = false;
  for (const auto& capability : target.capabilities) {
    const auto key = capability_key(
        capability.capability_id,
        capability.capability_definition_revision);
    const auto [selected_revision, inserted_revision] =
        selected_revisions_by_id.emplace(
            capability.capability_id.value(),
            capability.capability_definition_revision.value());
    if (!inserted_revision &&
        selected_revision->second !=
            capability.capability_definition_revision.value()) {
      fail("component-type command targets select more than one revision of "
           "one capability ID");
    }
    const auto& definition =
        require_qualified_capability(qualified_capabilities, key).value();
    if (definition.scope != core::CapabilityScope::kTargetBinding) {
      fail("component-type command target selects a capability outside "
           "target_binding scope");
    }
    requires_control_binding =
        requires_control_binding ||
        definition.provider_binding_kind ==
            core::CapabilityProviderBindingKind::kControlBinding;
    selected_capabilities.insert(key);
    if (selected_capabilities.size() >
        core::kMaxComponentTypeCapabilityDependencies) {
      fail("component-type distinct capability dependency union exceeds its "
           "fixed bound");
    }
  }
  if (requires_control_binding && target.resource_roles.empty()) {
    fail("component-type target with a control-binding capability requires "
         "at least one resource role");
  }
}

void validate_semantics(
    const core::ComponentTypeDTO& type,
    std::span<const QualifiedCapabilityDefinition> capabilities) {
  // A machine owner (CONTRACTS §16, tag `machine`) has neither: it holds the
  // machine's connection and process, its members the targets. Neither has
  // a sensor (tag `sensor`): it only observes.
  const bool target_less =
      std::any_of(type.tags.begin(), type.tags.end(), [](const auto &tag) {
        return tag == "machine" || tag == "sensor";
      });
  if (type.model_roles.empty() && type.command_targets.empty() &&
      !target_less) {
    fail("component-type cannot be completely empty");
  }

  std::set<std::string> model_roles;
  for (const auto& role : type.model_roles) {
    if (!model_roles.insert(role.model_role_id.value()).second) {
      fail("component-type model_roles contains a duplicate model role");
    }
  }
  validate_structure(type, model_roles);

  if (!type.attachment_interfaces.empty() && type.tags.empty()) {
    fail("component-type with attachment interfaces requires at least one "
         "component tag");
  }
  for (const auto& interface : type.attachment_interfaces) {
    require_model_role(model_roles, interface.model_role_id,
                       "attachment interface");
    if (interface.accepts_tags.empty()) {
      fail("component-type attachment interface requires accepts_tags");
    }
  }

  const auto capability_index = build_capability_index(capabilities);
  std::set<std::string> target_ids;
  std::set<CapabilityKey> selected_capabilities;
  std::map<std::string, std::string, std::less<>>
      selected_revisions_by_id;
  for (const auto& target : type.command_targets) {
    if (!target_ids.insert(target.command_target_id.value()).second) {
      fail("component-type command_targets contains a duplicate target ID");
    }
    validate_target(target, model_roles, capability_index,
                    selected_capabilities, selected_revisions_by_id);
  }
  if (selected_capabilities.size() != capability_index.size()) {
    fail("qualified capability dependency view is not the exact selected "
         "closure");
  }
}

}  // namespace

QualifiedComponentType resolve_and_qualify_component_type(
    const core::ComponentTypeCatalogRecordRefDTO& reference,
    const core::ContentAddressedStoreKey& resolved_store_key,
    std::string_view stored_bytes,
    std::span<const QualifiedCapabilityDefinition> qualified_capabilities) {
  UnqualifiedSchemaNormalizedArtifactCandidate normalized =
      [&reference, &resolved_store_key, stored_bytes]() {
        try {
          return detail::resolve_and_normalize_exact_catalog_referent(
              reference, resolved_store_key, stored_bytes,
              core::kComponentTypeApiVersion, "component type");
        } catch (const detail::ExactCatalogReferentError& error) {
          fail(error.what());
        }
      }();

  const detail::ContentIdentitySpec identity_spec{
      detail::catalog_record_identity_domain("component_type"),
      std::string(core::kComponentTypeApiVersion),
      "component_type_revision"};
  if (!detail::verify_content_identity(normalized.normalized_root(),
                                       identity_spec)) {
    fail("component_type_revision does not match normalized semantic content");
  }

  auto decoded = decode_normalized(normalized.normalized_root());
  validate_semantics(decoded, qualified_capabilities);
  if (decoded.component_type_id != reference.record_id ||
      decoded.component_type_revision != reference.record_revision) {
    fail("component-type typed ID/revision selector does not match its "
         "referent");
  }
  return QualifiedComponentType(reference, std::move(decoded),
                                normalized.normalized_root());
}

}  // namespace manyforge::deployment
