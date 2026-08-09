#pragma once

#include <cstddef>
#include <cstdint>
#include <manyforge_core/types/coordinate_state.hpp>
#include <manyforge_core/types/placement_pose.hpp>
#include <manyforge_core/types/strong_id.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace manyforge::core {

inline constexpr std::string_view kComponentTypeApiVersion =
    "manyforge.component_type.v1";
inline constexpr std::size_t kMaxComponentTypeTags = 256U;
inline constexpr std::size_t kMaxComponentTypeModelRoles = 256U;
inline constexpr std::size_t kMaxComponentTypeModelMounts = 255U;
inline constexpr std::size_t kMaxComponentTypeCommandTargets = 256U;
inline constexpr std::size_t kMaxComponentTypeAttachmentInterfaces = 256U;
inline constexpr std::size_t kMaxComponentTypeTargetRoleAliases = 4096U;
inline constexpr std::size_t kMaxComponentTypeTargetCapabilities = 256U;
inline constexpr std::size_t kMaxComponentTypeCapabilityDependencies = 256U;
inline constexpr std::uint64_t kMaxComponentTypeAttachmentsPerInterface =
    4096U;

enum class ComponentTypeModelRoleCardinality : std::uint8_t {
  kExactlyOne = 0,
};

enum class ComponentTypeResourceSourceKind : std::uint8_t {
  kModelRole = 0,
  kComponentLocal,
};

[[nodiscard]] constexpr std::string_view to_string(
    ComponentTypeModelRoleCardinality value) noexcept {
  switch (value) {
    case ComponentTypeModelRoleCardinality::kExactlyOne:
      return "exactly_one";
  }
  return {};
}

[[nodiscard]] constexpr std::string_view to_string(
    ComponentTypeResourceSourceKind value) noexcept {
  switch (value) {
    case ComponentTypeResourceSourceKind::kModelRole:
      return "model_role";
    case ComponentTypeResourceSourceKind::kComponentLocal:
      return "component_local";
  }
  return {};
}

struct ComponentTypeModelRoleDTO {
  ModelRoleId model_role_id;
  ComponentTypeModelRoleCardinality cardinality;

  friend bool operator==(const ComponentTypeModelRoleDTO&,
                         const ComponentTypeModelRoleDTO&) = default;
};

struct ComponentTypeModelFrameRefDTO {
  ModelRoleId model_role_id;
  FrameRoleId frame_role_id;

  friend bool operator==(const ComponentTypeModelFrameRefDTO&,
                         const ComponentTypeModelFrameRefDTO&) = default;
};

struct ComponentTypeModelMountDTO {
  ComponentTypeModelFrameRefDTO parent;
  ComponentTypeModelFrameRefDTO child;
  PlacementPoseDTO child_frame_pose_in_parent_frame;

  friend bool operator==(const ComponentTypeModelMountDTO&,
                         const ComponentTypeModelMountDTO&) = default;
};

/// One target-local group alias selecting an asset-scoped group role.
struct ComponentTypeModelGroupRoleDTO {
  ModelGroupRoleId model_group_role_id;
  ModelRoleId model_role_id;
  ModelGroupRoleId asset_model_group_role_id;

  friend bool operator==(const ComponentTypeModelGroupRoleDTO&,
                         const ComponentTypeModelGroupRoleDTO&) = default;
};

/// Model-backed branch of one ordered target-local resource alias.
struct ComponentTypeModelResourceRoleDTO {
  ResourceRoleId resource_role_id;
  ModelRoleId model_role_id;
  ResourceRoleId asset_resource_role_id;

  friend bool operator==(const ComponentTypeModelResourceRoleDTO&,
                         const ComponentTypeModelResourceRoleDTO&) = default;
};

/// Component-local branch of one ordered target-local resource alias.
///
/// The graph/compiler later binds this logical resource directly without
/// inventing a model role, asset role, joint, or model asset.
struct ComponentTypeLocalResourceRoleDTO {
  ResourceRoleId resource_role_id;
  std::optional<CoordinateKind> coordinate_kind;

  friend bool operator==(const ComponentTypeLocalResourceRoleDTO&,
                         const ComponentTypeLocalResourceRoleDTO&) = default;
};

/// Closed in-memory union corresponding to the flat `source_kind` wire union.
using ComponentTypeResourceRoleDTO =
    std::variant<ComponentTypeModelResourceRoleDTO,
                 ComponentTypeLocalResourceRoleDTO>;

[[nodiscard]] constexpr ComponentTypeResourceSourceKind resource_source_kind(
    const ComponentTypeResourceRoleDTO& value) noexcept {
  return std::holds_alternative<ComponentTypeModelResourceRoleDTO>(value)
             ? ComponentTypeResourceSourceKind::kModelRole
             : ComponentTypeResourceSourceKind::kComponentLocal;
}

[[nodiscard]] inline ResourceRoleId resource_role_id(
    const ComponentTypeResourceRoleDTO& value) {
  return std::visit(
      [](const auto& role) -> ResourceRoleId {
        return role.resource_role_id;
      },
      value);
}

struct ComponentTypeFrameRoleDTO {
  FrameRoleId frame_role_id;
  ModelRoleId model_role_id;
  FrameRoleId asset_frame_role_id;

  friend bool operator==(const ComponentTypeFrameRoleDTO&,
                         const ComponentTypeFrameRoleDTO&) = default;
};

struct ComponentTypeCollisionBodyRoleDTO {
  CollisionBodyRoleId collision_body_role_id;
  ModelRoleId model_role_id;
  CollisionBodyRoleId asset_collision_body_role_id;

  friend bool operator==(const ComponentTypeCollisionBodyRoleDTO&,
                         const ComponentTypeCollisionBodyRoleDTO&) = default;
};

struct ComponentTypeCapabilityDTO {
  CapabilityId capability_id;
  CapabilityDefinitionRevision capability_definition_revision;

  friend bool operator==(const ComponentTypeCapabilityDTO&,
                         const ComponentTypeCapabilityDTO&) = default;
};

struct ComponentTypeCommandTargetDTO {
  CommandTargetId command_target_id;
  std::vector<ComponentTypeModelGroupRoleDTO> model_group_roles;
  std::vector<ComponentTypeResourceRoleDTO> resource_roles;
  std::vector<ComponentTypeFrameRoleDTO> frame_roles;
  std::vector<ComponentTypeCollisionBodyRoleDTO> collision_body_roles;
  std::vector<ComponentTypeCapabilityDTO> capabilities;

  friend bool operator==(const ComponentTypeCommandTargetDTO&,
                         const ComponentTypeCommandTargetDTO&) = default;
};

struct ComponentTypeAttachmentInterfaceDTO {
  AttachmentInterfaceId attachment_interface_id;
  ModelRoleId model_role_id;
  FrameRoleId frame_role_id;
  std::vector<std::string> accepts_tags;
  std::uint64_t max_attachments = 1U;

  friend bool operator==(const ComponentTypeAttachmentInterfaceDTO&,
                         const ComponentTypeAttachmentInterfaceDTO&) = default;
};

/// Exact normalized payload for `manyforge.component_type.v1`.
///
/// This declaration contains reusable logical structure and command-target
/// aliases only. It contains no model selectors, controller/topology policy,
/// runtime instances, generated names, or observed state. Model-asset source
/// role existence and complete control compatibility are compiler concerns.
struct ComponentTypeDTO {
  ComponentTypeId component_type_id;
  ComponentTypeRevision component_type_revision;
  std::vector<std::string> tags;
  std::vector<ComponentTypeModelRoleDTO> model_roles;
  std::optional<ComponentTypeModelFrameRefDTO> component_root;
  std::vector<ComponentTypeModelMountDTO> model_mounts;
  std::vector<ComponentTypeCommandTargetDTO> command_targets;
  std::vector<ComponentTypeAttachmentInterfaceDTO> attachment_interfaces;

  friend bool operator==(const ComponentTypeDTO&,
                         const ComponentTypeDTO&) = default;
};

}  // namespace manyforge::core
