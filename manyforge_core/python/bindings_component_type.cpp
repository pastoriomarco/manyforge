#include <pybind11/operators.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <cstdint>
#include <manyforge_core/types/component_type.hpp>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "binding_registrations.hpp"

namespace py = pybind11;

namespace manyforge::core::python {
namespace {

[[nodiscard]] ComponentTypeResourceRoleDTO resource_role_from_python(
    const py::handle& value) {
  if (py::isinstance<ComponentTypeModelResourceRoleDTO>(value)) {
    return value.cast<ComponentTypeModelResourceRoleDTO>();
  }
  if (py::isinstance<ComponentTypeLocalResourceRoleDTO>(value)) {
    return value.cast<ComponentTypeLocalResourceRoleDTO>();
  }
  throw py::type_error(
      "resource role must be ComponentTypeModelResourceRoleDTO or "
      "ComponentTypeLocalResourceRoleDTO");
}

[[nodiscard]] std::vector<ComponentTypeResourceRoleDTO>
resource_roles_from_python(const py::iterable& values) {
  std::vector<ComponentTypeResourceRoleDTO> result;
  for (const auto& value : values) {
    result.push_back(resource_role_from_python(value));
  }
  return result;
}

[[nodiscard]] py::list resource_roles_to_python(
    const std::vector<ComponentTypeResourceRoleDTO>& values) {
  py::list result;
  for (const auto& value : values) {
    std::visit(
        [&result](const auto& branch) {
          result.append(
              py::cast(branch, py::return_value_policy::copy));
        },
        value);
  }
  return result;
}

}  // namespace

void register_component_type_bindings(py::module_& module) {
  py::enum_<ComponentTypeModelRoleCardinality>(
      module, "ComponentTypeModelRoleCardinality")
      .value("kExactlyOne",
             ComponentTypeModelRoleCardinality::kExactlyOne);

  py::enum_<ComponentTypeResourceSourceKind>(
      module, "ComponentTypeResourceSourceKind")
      .value("kModelRole", ComponentTypeResourceSourceKind::kModelRole)
      .value("kComponentLocal",
             ComponentTypeResourceSourceKind::kComponentLocal);

  py::class_<ComponentTypeModelRoleDTO>(
      module, "ComponentTypeModelRoleDTO", py::is_final())
      .def(py::init([](ModelRoleId model_role_id,
                       ComponentTypeModelRoleCardinality cardinality) {
             return ComponentTypeModelRoleDTO{
                 std::move(model_role_id), cardinality};
           }),
           py::arg("model_role_id"), py::arg("cardinality"))
      .def_readonly("model_role_id",
                    &ComponentTypeModelRoleDTO::model_role_id)
      .def_readonly("cardinality",
                    &ComponentTypeModelRoleDTO::cardinality)
      .def(py::self == py::self)
      .def(py::self != py::self);

  py::class_<ComponentTypeModelFrameRefDTO>(
      module, "ComponentTypeModelFrameRefDTO", py::is_final())
      .def(py::init([](ModelRoleId model_role_id,
                       FrameRoleId frame_role_id) {
             return ComponentTypeModelFrameRefDTO{
                 std::move(model_role_id), std::move(frame_role_id)};
           }),
           py::arg("model_role_id"), py::arg("frame_role_id"))
      .def_readonly("model_role_id",
                    &ComponentTypeModelFrameRefDTO::model_role_id)
      .def_readonly("frame_role_id",
                    &ComponentTypeModelFrameRefDTO::frame_role_id)
      .def(py::self == py::self)
      .def(py::self != py::self);

  py::class_<ComponentTypeModelMountDTO>(
      module, "ComponentTypeModelMountDTO", py::is_final())
      .def(py::init([](
               ComponentTypeModelFrameRefDTO parent,
               ComponentTypeModelFrameRefDTO child,
               PlacementPoseDTO child_frame_pose_in_parent_frame) {
             return ComponentTypeModelMountDTO{
                 std::move(parent), std::move(child),
                 std::move(child_frame_pose_in_parent_frame)};
           }),
           py::arg("parent"), py::arg("child"),
           py::arg("child_frame_pose_in_parent_frame"))
      .def_readonly("parent", &ComponentTypeModelMountDTO::parent)
      .def_readonly("child", &ComponentTypeModelMountDTO::child)
      .def_readonly(
          "child_frame_pose_in_parent_frame",
          &ComponentTypeModelMountDTO::child_frame_pose_in_parent_frame)
      .def(py::self == py::self)
      .def(py::self != py::self);

  py::class_<ComponentTypeModelGroupRoleDTO>(
      module, "ComponentTypeModelGroupRoleDTO", py::is_final())
      .def(py::init([](
               ModelGroupRoleId model_group_role_id,
               ModelRoleId model_role_id,
               ModelGroupRoleId asset_model_group_role_id) {
             return ComponentTypeModelGroupRoleDTO{
                 std::move(model_group_role_id),
                 std::move(model_role_id),
                 std::move(asset_model_group_role_id)};
           }),
           py::arg("model_group_role_id"), py::arg("model_role_id"),
           py::arg("asset_model_group_role_id"))
      .def_readonly("model_group_role_id",
                    &ComponentTypeModelGroupRoleDTO::model_group_role_id)
      .def_readonly("model_role_id",
                    &ComponentTypeModelGroupRoleDTO::model_role_id)
      .def_readonly(
          "asset_model_group_role_id",
          &ComponentTypeModelGroupRoleDTO::asset_model_group_role_id)
      .def(py::self == py::self)
      .def(py::self != py::self);

  py::class_<ComponentTypeModelResourceRoleDTO>(
      module, "ComponentTypeModelResourceRoleDTO", py::is_final())
      .def(py::init([](
               ResourceRoleId resource_role_id, ModelRoleId model_role_id,
               ResourceRoleId asset_resource_role_id) {
             return ComponentTypeModelResourceRoleDTO{
                 std::move(resource_role_id), std::move(model_role_id),
                 std::move(asset_resource_role_id)};
           }),
           py::arg("resource_role_id"), py::arg("model_role_id"),
           py::arg("asset_resource_role_id"))
      .def_readonly("resource_role_id",
                    &ComponentTypeModelResourceRoleDTO::resource_role_id)
      .def_readonly("model_role_id",
                    &ComponentTypeModelResourceRoleDTO::model_role_id)
      .def_readonly(
          "asset_resource_role_id",
          &ComponentTypeModelResourceRoleDTO::asset_resource_role_id)
      .def(py::self == py::self)
      .def(py::self != py::self);

  py::class_<ComponentTypeLocalResourceRoleDTO>(
      module, "ComponentTypeLocalResourceRoleDTO", py::is_final())
      .def(py::init([](ResourceRoleId resource_role_id,
                       std::optional<CoordinateKind> coordinate_kind) {
             return ComponentTypeLocalResourceRoleDTO{
                 std::move(resource_role_id), coordinate_kind};
           }),
           py::arg("resource_role_id"),
           py::arg("coordinate_kind") = py::none())
      .def_readonly("resource_role_id",
                    &ComponentTypeLocalResourceRoleDTO::resource_role_id)
      .def_readonly("coordinate_kind",
                    &ComponentTypeLocalResourceRoleDTO::coordinate_kind)
      .def(py::self == py::self)
      .def(py::self != py::self);

  py::tuple resource_role_types{2U};
  resource_role_types[0] =
      module.attr("ComponentTypeModelResourceRoleDTO");
  resource_role_types[1] =
      module.attr("ComponentTypeLocalResourceRoleDTO");
  module.attr("ComponentTypeResourceRoleDTO") =
      py::module_::import("typing")
          .attr("Union")
          .attr("__getitem__")(resource_role_types);

  py::class_<ComponentTypeFrameRoleDTO>(
      module, "ComponentTypeFrameRoleDTO", py::is_final())
      .def(py::init([](FrameRoleId frame_role_id,
                       ModelRoleId model_role_id,
                       FrameRoleId asset_frame_role_id) {
             return ComponentTypeFrameRoleDTO{
                 std::move(frame_role_id), std::move(model_role_id),
                 std::move(asset_frame_role_id)};
           }),
           py::arg("frame_role_id"), py::arg("model_role_id"),
           py::arg("asset_frame_role_id"))
      .def_readonly("frame_role_id",
                    &ComponentTypeFrameRoleDTO::frame_role_id)
      .def_readonly("model_role_id",
                    &ComponentTypeFrameRoleDTO::model_role_id)
      .def_readonly("asset_frame_role_id",
                    &ComponentTypeFrameRoleDTO::asset_frame_role_id)
      .def(py::self == py::self)
      .def(py::self != py::self);

  py::class_<ComponentTypeCollisionBodyRoleDTO>(
      module, "ComponentTypeCollisionBodyRoleDTO", py::is_final())
      .def(py::init([](
               CollisionBodyRoleId collision_body_role_id,
               ModelRoleId model_role_id,
               CollisionBodyRoleId asset_collision_body_role_id) {
             return ComponentTypeCollisionBodyRoleDTO{
                 std::move(collision_body_role_id),
                 std::move(model_role_id),
                 std::move(asset_collision_body_role_id)};
           }),
           py::arg("collision_body_role_id"), py::arg("model_role_id"),
           py::arg("asset_collision_body_role_id"))
      .def_readonly(
          "collision_body_role_id",
          &ComponentTypeCollisionBodyRoleDTO::collision_body_role_id)
      .def_readonly("model_role_id",
                    &ComponentTypeCollisionBodyRoleDTO::model_role_id)
      .def_readonly(
          "asset_collision_body_role_id",
          &ComponentTypeCollisionBodyRoleDTO::
              asset_collision_body_role_id)
      .def(py::self == py::self)
      .def(py::self != py::self);

  py::class_<ComponentTypeCapabilityDTO>(
      module, "ComponentTypeCapabilityDTO", py::is_final())
      .def(py::init([](
               CapabilityId capability_id,
               CapabilityDefinitionRevision capability_definition_revision) {
             return ComponentTypeCapabilityDTO{
                 std::move(capability_id),
                 std::move(capability_definition_revision)};
           }),
           py::arg("capability_id"),
           py::arg("capability_definition_revision"))
      .def_readonly("capability_id",
                    &ComponentTypeCapabilityDTO::capability_id)
      .def_readonly(
          "capability_definition_revision",
          &ComponentTypeCapabilityDTO::capability_definition_revision)
      .def(py::self == py::self)
      .def(py::self != py::self);

  py::class_<ComponentTypeCommandTargetDTO>(
      module, "ComponentTypeCommandTargetDTO", py::is_final())
      .def(py::init([](
               CommandTargetId command_target_id,
               std::vector<ComponentTypeModelGroupRoleDTO>
                   model_group_roles,
               const py::iterable& resource_roles,
               std::vector<ComponentTypeFrameRoleDTO> frame_roles,
               std::vector<ComponentTypeCollisionBodyRoleDTO>
                   collision_body_roles,
               std::vector<ComponentTypeCapabilityDTO> capabilities) {
             return ComponentTypeCommandTargetDTO{
                 std::move(command_target_id),
                 std::move(model_group_roles),
                 resource_roles_from_python(resource_roles),
                 std::move(frame_roles),
                 std::move(collision_body_roles),
                 std::move(capabilities)};
           }),
           py::arg("command_target_id"), py::arg("model_group_roles"),
           py::arg("resource_roles"), py::arg("frame_roles"),
           py::arg("collision_body_roles"), py::arg("capabilities"))
      .def_readonly("command_target_id",
                    &ComponentTypeCommandTargetDTO::command_target_id)
      .def_readonly("model_group_roles",
                    &ComponentTypeCommandTargetDTO::model_group_roles)
      .def_property_readonly(
          "resource_roles",
          [](const ComponentTypeCommandTargetDTO& value) {
            return resource_roles_to_python(value.resource_roles);
          })
      .def_readonly("frame_roles",
                    &ComponentTypeCommandTargetDTO::frame_roles)
      .def_readonly(
          "collision_body_roles",
          &ComponentTypeCommandTargetDTO::collision_body_roles)
      .def_readonly("capabilities",
                    &ComponentTypeCommandTargetDTO::capabilities)
      .def(py::self == py::self)
      .def(py::self != py::self);

  py::class_<ComponentTypeAttachmentInterfaceDTO>(
      module, "ComponentTypeAttachmentInterfaceDTO", py::is_final())
      .def(py::init([](
               AttachmentInterfaceId attachment_interface_id,
               ModelRoleId model_role_id, FrameRoleId frame_role_id,
               std::vector<std::string> accepts_tags,
               std::uint64_t max_attachments) {
             return ComponentTypeAttachmentInterfaceDTO{
                 std::move(attachment_interface_id),
                 std::move(model_role_id), std::move(frame_role_id),
                 std::move(accepts_tags), max_attachments};
           }),
           py::arg("attachment_interface_id"),
           py::arg("model_role_id"), py::arg("frame_role_id"),
           py::arg("accepts_tags"), py::arg("max_attachments") = 1U)
      .def_readonly(
          "attachment_interface_id",
          &ComponentTypeAttachmentInterfaceDTO::attachment_interface_id)
      .def_readonly(
          "model_role_id",
          &ComponentTypeAttachmentInterfaceDTO::model_role_id)
      .def_readonly(
          "frame_role_id",
          &ComponentTypeAttachmentInterfaceDTO::frame_role_id)
      .def_readonly(
          "accepts_tags",
          &ComponentTypeAttachmentInterfaceDTO::accepts_tags)
      .def_readonly(
          "max_attachments",
          &ComponentTypeAttachmentInterfaceDTO::max_attachments)
      .def(py::self == py::self)
      .def(py::self != py::self);

  py::class_<ComponentTypeDTO>(module, "ComponentTypeDTO",
                               py::is_final())
      .def(py::init([](
               ComponentTypeId component_type_id,
               ComponentTypeRevision component_type_revision,
               std::vector<std::string> tags,
               std::vector<ComponentTypeModelRoleDTO> model_roles,
               std::optional<ComponentTypeModelFrameRefDTO> component_root,
               std::vector<ComponentTypeModelMountDTO> model_mounts,
               std::vector<ComponentTypeCommandTargetDTO> command_targets,
               std::vector<ComponentTypeAttachmentInterfaceDTO>
                   attachment_interfaces) {
             return ComponentTypeDTO{
                 std::move(component_type_id),
                 std::move(component_type_revision), std::move(tags),
                 std::move(model_roles), std::move(component_root),
                 std::move(model_mounts), std::move(command_targets),
                 std::move(attachment_interfaces)};
           }),
           py::arg("component_type_id"),
           py::arg("component_type_revision"), py::arg("tags"),
           py::arg("model_roles"), py::arg("component_root"),
           py::arg("model_mounts"), py::arg("command_targets"),
           py::arg("attachment_interfaces"))
      .def_readonly("component_type_id",
                    &ComponentTypeDTO::component_type_id)
      .def_readonly("component_type_revision",
                    &ComponentTypeDTO::component_type_revision)
      .def_readonly("tags", &ComponentTypeDTO::tags)
      .def_readonly("model_roles", &ComponentTypeDTO::model_roles)
      .def_readonly("component_root",
                    &ComponentTypeDTO::component_root)
      .def_readonly("model_mounts", &ComponentTypeDTO::model_mounts)
      .def_readonly("command_targets",
                    &ComponentTypeDTO::command_targets)
      .def_readonly("attachment_interfaces",
                    &ComponentTypeDTO::attachment_interfaces)
      .def(py::self == py::self)
      .def(py::self != py::self);

  module.attr("COMPONENT_TYPE_API_VERSION") =
      std::string(kComponentTypeApiVersion);
  module.attr("MAX_COMPONENT_TYPE_TAGS") = kMaxComponentTypeTags;
  module.attr("MAX_COMPONENT_TYPE_MODEL_ROLES") =
      kMaxComponentTypeModelRoles;
  module.attr("MAX_COMPONENT_TYPE_MODEL_MOUNTS") =
      kMaxComponentTypeModelMounts;
  module.attr("MAX_COMPONENT_TYPE_COMMAND_TARGETS") =
      kMaxComponentTypeCommandTargets;
  module.attr("MAX_COMPONENT_TYPE_ATTACHMENT_INTERFACES") =
      kMaxComponentTypeAttachmentInterfaces;
  module.attr("MAX_COMPONENT_TYPE_TARGET_ROLE_ALIASES") =
      kMaxComponentTypeTargetRoleAliases;
  module.attr("MAX_COMPONENT_TYPE_TARGET_CAPABILITIES") =
      kMaxComponentTypeTargetCapabilities;
  module.attr("MAX_COMPONENT_TYPE_CAPABILITY_DEPENDENCIES") =
      kMaxComponentTypeCapabilityDependencies;
  module.attr("MAX_COMPONENT_TYPE_ATTACHMENTS_PER_INTERFACE") =
      kMaxComponentTypeAttachmentsPerInterface;
  module.def(
      "component_type_model_role_cardinality_to_string",
      [](ComponentTypeModelRoleCardinality value) {
        return std::string(to_string(value));
      },
      py::arg("value"));
  module.def(
      "component_type_resource_source_kind_to_string",
      [](ComponentTypeResourceSourceKind value) {
        return std::string(to_string(value));
      },
      py::arg("value"));
  module.def(
      "component_type_resource_source_kind",
      [](const py::object& value) {
        return resource_source_kind(resource_role_from_python(value));
      },
      py::arg("value"));
  module.def(
      "component_type_resource_role_id",
      [](const py::object& value) -> ResourceRoleId {
        const auto resource = resource_role_from_python(value);
        return resource_role_id(resource);
      },
      py::arg("value"));
}

}  // namespace manyforge::core::python
