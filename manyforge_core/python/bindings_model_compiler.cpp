#include <pybind11/operators.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <manyforge_core/types/cell_graph.hpp>
#include <manyforge_core/types/model_asset.hpp>
#include <manyforge_deployment/cell_structure.hpp>
#include <manyforge_deployment/model_source.hpp>

#include "binding_registrations.hpp"

namespace py = pybind11;

namespace manyforge::core::python {
namespace {

using ExactRevisionKey = std::pair<std::string, std::string>;

template <typename Values>
[[nodiscard]] py::tuple immutable_snapshot(const Values& values) {
  py::tuple snapshot{values.size()};
  for (std::size_t index = 0U; index < values.size(); ++index) {
    snapshot[index] = py::cast(values[index]);
  }
  return snapshot;
}

[[nodiscard]] std::vector<
    deployment::ResolvedModelSourceArtifactCandidate>
bounded_source_closure_from_python(
    const deployment::ModelSourceRetrievalPlan& retrieval_plan,
    const py::iterable& resolved_artifacts) {
  const auto& planned = retrieval_plan.artifacts();
  std::map<std::string,
           const deployment::ModelSourceRetrievalPlanRow*>
      planned_by_uri;
  for (const auto& artifact : planned) {
    planned_by_uri.emplace(artifact.logical_uri, &artifact);
  }

  std::set<std::string> seen;
  std::vector<deployment::ResolvedModelSourceArtifactCandidate> bounded;
  bounded.reserve(planned.size());
  for (const auto& item : resolved_artifacts) {
    if (bounded.size() == planned.size()) {
      throw deployment::ModelSourceError(
          "resolved model source contains more artifacts than declared");
    }
    if (!py::isinstance<
            deployment::ResolvedModelSourceArtifactCandidate>(item)) {
      throw py::type_error(
          "resolved_artifacts must contain only "
          "ResolvedModelSourceArtifactCandidate values");
    }
    const auto& candidate =
        item.cast<
            const deployment::ResolvedModelSourceArtifactCandidate&>();
    const auto expected = planned_by_uri.find(candidate.logical_uri);
    if (expected == planned_by_uri.end()) {
      throw deployment::ModelSourceError(
          "resolved model source contains an undeclared logical URI");
    }
    if (!seen.insert(candidate.logical_uri).second) {
      throw deployment::ModelSourceError(
          "resolved model source contains a duplicate logical URI");
    }

    const auto& declaration = *expected->second;
    if (candidate.resolved_store_key.value() !=
        declaration.expected_store_key.value()) {
      throw deployment::ModelSourceError(
          "resolved model source store key does not match its digest");
    }
    if (candidate.stored_bytes.size() != declaration.byte_size) {
      throw deployment::ModelSourceError(
          "resolved model source byte size does not match its declaration");
    }
    bounded.push_back(candidate);
  }
  return bounded;
}

[[nodiscard]] std::set<ExactRevisionKey> selected_model_assets(
    const deployment::GraphRevisionVerifiedCellGraph& graph) {
  std::set<ExactRevisionKey> selected;
  for (const auto& instance : graph.graph().model_instances) {
    selected.emplace(instance.model_asset_id.value(),
                     instance.model_revision.value());
  }
  return selected;
}

[[nodiscard]] std::set<ExactRevisionKey> selected_component_types(
    const deployment::GraphRevisionVerifiedCellGraph& graph) {
  std::set<ExactRevisionKey> selected;
  for (const auto& component : graph.graph().controllable_components) {
    selected.emplace(component.component_type_id.value(),
                     component.component_type_revision.value());
  }
  return selected;
}

template <typename Qualified, typename KeyFunction>
[[nodiscard]] std::vector<Qualified> bounded_exact_closure_from_python(
    const py::iterable& values,
    const std::set<ExactRevisionKey>& selected,
    std::string_view argument_name, std::string_view record_name,
    KeyFunction key_function) {
  std::set<ExactRevisionKey> seen;
  std::vector<Qualified> bounded;
  bounded.reserve(selected.size());
  for (const auto& item : values) {
    if (bounded.size() == selected.size()) {
      throw deployment::CellStructureError(
          std::string(argument_name) +
          " contains more values than the graph selects");
    }
    if (!py::isinstance<Qualified>(item)) {
      throw py::type_error(std::string(argument_name) +
                           " must contain only " +
                           std::string(record_name) + " values");
    }
    const auto& qualified = item.cast<const Qualified&>();
    const auto key = key_function(qualified);
    if (!selected.contains(key)) {
      const bool selected_id = std::any_of(
          selected.begin(), selected.end(),
          [&key](const auto& selected_key) {
            return selected_key.first == key.first;
          });
      throw deployment::CellStructureError(
          std::string(selected_id ? "stale " : "unused extra ") +
          std::string(record_name) + " '" + key.first +
          "' at revision '" + key.second + "'");
    }
    if (!seen.insert(key).second) {
      throw deployment::CellStructureError(
          "duplicate exact " + std::string(record_name) + " '" +
          key.first + "' at revision '" + key.second + "'");
    }
    bounded.push_back(qualified);
  }
  return bounded;
}

}  // namespace

void register_model_compiler_bindings(py::module_& module) {
  using deployment::ParsedModelSourceIr;
  using deployment::ParsedSrdfChain;
  using deployment::ParsedSrdfDisabledCollision;
  using deployment::ParsedSrdfEndEffector;
  using deployment::ParsedSrdfGroup;
  using deployment::ParsedSrdfGroupState;
  using deployment::ParsedSrdfGroupStateJoint;
  using deployment::ParsedUrdfJoint;
  using deployment::ModelSourceRetrievalPlan;
  using deployment::ModelSourceRetrievalPlanRow;
  using deployment::ResolvedModelSourceArtifactCandidate;
  using deployment::SourceVerifiedModelAsset;
  using deployment::StructurallyQualifiedCellGraph;
  using deployment::StructuralAttachmentEndpointResolution;
  using deployment::StructuralAttachmentInterfaceResolution;
  using deployment::StructuralAttachmentResolution;
  using deployment::StructuralCollisionBodyResolution;
  using deployment::StructuralCommandTargetResolution;
  using deployment::StructuralComponentLocalResourceResolution;
  using deployment::StructuralComponentResolution;
  using deployment::StructuralFrameResolution;
  using deployment::StructuralModelGroupResolution;
  using deployment::StructuralModelInstanceResolution;
  using deployment::StructuralModelMountResolution;
  using deployment::StructuralModelOccurrence;
  using deployment::StructuralModelResourceResolution;
  using deployment::StructuralTargetFrameResolution;
  using deployment::VerifiedModelSourceArtifact;

  py::register_exception<deployment::ModelSourceError>(
      module, "ModelSourceError", PyExc_ValueError);
  py::register_exception<deployment::CellStructureError>(
      module, "CellStructureError", PyExc_ValueError);

  py::enum_<deployment::ModelSourceArtifactKind>(
      module, "ModelSourceArtifactKind")
      .value("kUrdfXmlEntrypoint",
             deployment::ModelSourceArtifactKind::kUrdfXmlEntrypoint)
      .value("kSrdfXmlEntrypoint",
             deployment::ModelSourceArtifactKind::kSrdfXmlEntrypoint)
      .value("kStlMesh",
             deployment::ModelSourceArtifactKind::kStlMesh)
      .value("kPngTexture",
             deployment::ModelSourceArtifactKind::kPngTexture)
      .value("kJpegTexture",
             deployment::ModelSourceArtifactKind::kJpegTexture);

  py::enum_<deployment::ParsedUrdfJointType>(
      module, "ParsedUrdfJointType")
      .value("kRevolute", deployment::ParsedUrdfJointType::kRevolute)
      .value("kContinuous",
             deployment::ParsedUrdfJointType::kContinuous)
      .value("kPrismatic", deployment::ParsedUrdfJointType::kPrismatic)
      .value("kFixed", deployment::ParsedUrdfJointType::kFixed)
      .value("kFloating", deployment::ParsedUrdfJointType::kFloating)
      .value("kPlanar", deployment::ParsedUrdfJointType::kPlanar);

  py::enum_<deployment::StructuralPlacementSource>(
      module, "StructuralPlacementSource")
      .value("kDirectModelPoseInCell",
             deployment::StructuralPlacementSource::
                 kDirectModelPoseInCell)
      .value("kDirectComponentPoseInCell",
             deployment::StructuralPlacementSource::
                 kDirectComponentPoseInCell)
      .value("kDerivedFromComponentAttachment",
             deployment::StructuralPlacementSource::
                 kDerivedFromComponentAttachment)
      .value("kDerivedFromComponentModelMount",
             deployment::StructuralPlacementSource::
                 kDerivedFromComponentModelMount);

  py::class_<ModelSourceRetrievalPlanRow>(
      module, "ModelSourceRetrievalPlanRow", py::is_final())
      .def_readonly("logical_uri",
                    &ModelSourceRetrievalPlanRow::logical_uri)
      .def_readonly("media_type",
                    &ModelSourceRetrievalPlanRow::media_type)
      .def_readonly("byte_size",
                    &ModelSourceRetrievalPlanRow::byte_size)
      .def_readonly("content_digest",
                    &ModelSourceRetrievalPlanRow::content_digest)
      .def_readonly("expected_store_key",
                    &ModelSourceRetrievalPlanRow::expected_store_key)
      .def_readonly("artifact_kind",
                    &ModelSourceRetrievalPlanRow::artifact_kind)
      .def(py::self == py::self)
      .def(py::self != py::self);

  py::class_<ModelSourceRetrievalPlan>(
      module, "ModelSourceRetrievalPlan", py::is_final())
      .def_property_readonly(
          "qualified_asset",
          &ModelSourceRetrievalPlan::qualified_asset)
      .def_property_readonly(
          "artifacts",
          [](const ModelSourceRetrievalPlan& plan) {
            return immutable_snapshot(plan.artifacts());
          })
      .def_property_readonly("total_bytes",
                             &ModelSourceRetrievalPlan::total_bytes);

  py::class_<ResolvedModelSourceArtifactCandidate>(
      module, "ResolvedModelSourceArtifactCandidate", py::is_final())
      .def(py::init([](std::string logical_uri,
                       ContentAddressedStoreKey resolved_store_key,
                       const py::bytes& stored_bytes) {
             return ResolvedModelSourceArtifactCandidate{
                 std::move(logical_uri),
                 std::move(resolved_store_key),
                 stored_bytes.cast<std::string>()};
           }),
           py::arg("logical_uri"), py::arg("resolved_store_key"),
           py::arg("stored_bytes"))
      .def_readonly("logical_uri",
                    &ResolvedModelSourceArtifactCandidate::logical_uri)
      .def_readonly(
          "resolved_store_key",
          &ResolvedModelSourceArtifactCandidate::resolved_store_key)
      .def_property_readonly(
          "stored_bytes",
          [](const ResolvedModelSourceArtifactCandidate& candidate) {
            return py::bytes(candidate.stored_bytes);
          });

  py::class_<VerifiedModelSourceArtifact>(
      module, "VerifiedModelSourceArtifact", py::is_final())
      .def_readonly("logical_uri",
                    &VerifiedModelSourceArtifact::logical_uri)
      .def_readonly("media_type",
                    &VerifiedModelSourceArtifact::media_type)
      .def_readonly("byte_size",
                    &VerifiedModelSourceArtifact::byte_size)
      .def_readonly("content_digest",
                    &VerifiedModelSourceArtifact::content_digest)
      .def_readonly("resolved_store_key",
                    &VerifiedModelSourceArtifact::resolved_store_key)
      .def(py::self == py::self)
      .def(py::self != py::self);

  py::class_<ParsedUrdfJoint>(module, "ParsedUrdfJoint",
                              py::is_final())
      .def_readonly("name", &ParsedUrdfJoint::name)
      .def_readonly("type", &ParsedUrdfJoint::type)
      .def_readonly("parent_link_name",
                    &ParsedUrdfJoint::parent_link_name)
      .def_readonly("child_link_name",
                    &ParsedUrdfJoint::child_link_name)
      .def_readonly("mimic_joint_name",
                    &ParsedUrdfJoint::mimic_joint_name)
      .def(py::self == py::self)
      .def(py::self != py::self);

  py::class_<ParsedSrdfChain>(module, "ParsedSrdfChain",
                              py::is_final())
      .def_readonly("base_link_name", &ParsedSrdfChain::base_link_name)
      .def_readonly("tip_link_name", &ParsedSrdfChain::tip_link_name)
      .def(py::self == py::self)
      .def(py::self != py::self);

  py::class_<ParsedSrdfGroup>(module, "ParsedSrdfGroup",
                              py::is_final())
      .def_readonly("name", &ParsedSrdfGroup::name)
      .def_property_readonly(
          "link_names",
          [](const ParsedSrdfGroup& group) {
            return immutable_snapshot(group.link_names);
          })
      .def_property_readonly(
          "joint_names",
          [](const ParsedSrdfGroup& group) {
            return immutable_snapshot(group.joint_names);
          })
      .def_property_readonly(
          "chains",
          [](const ParsedSrdfGroup& group) {
            return immutable_snapshot(group.chains);
          })
      .def_property_readonly(
          "subgroup_names",
          [](const ParsedSrdfGroup& group) {
            return immutable_snapshot(group.subgroup_names);
          })
      .def(py::self == py::self)
      .def(py::self != py::self);

  py::class_<ParsedSrdfGroupStateJoint>(
      module, "ParsedSrdfGroupStateJoint", py::is_final())
      .def_readonly("joint_name",
                    &ParsedSrdfGroupStateJoint::joint_name)
      .def_readonly("value", &ParsedSrdfGroupStateJoint::value)
      .def(py::self == py::self)
      .def(py::self != py::self);

  py::class_<ParsedSrdfGroupState>(
      module, "ParsedSrdfGroupState", py::is_final())
      .def_readonly("name", &ParsedSrdfGroupState::name)
      .def_readonly("group_name", &ParsedSrdfGroupState::group_name)
      .def_property_readonly(
          "joints",
          [](const ParsedSrdfGroupState& state) {
            return immutable_snapshot(state.joints);
          })
      .def(py::self == py::self)
      .def(py::self != py::self);

  py::class_<ParsedSrdfEndEffector>(
      module, "ParsedSrdfEndEffector", py::is_final())
      .def_readonly("name", &ParsedSrdfEndEffector::name)
      .def_readonly("parent_link_name",
                    &ParsedSrdfEndEffector::parent_link_name)
      .def_readonly("group_name",
                    &ParsedSrdfEndEffector::group_name)
      .def_readonly("parent_group_name",
                    &ParsedSrdfEndEffector::parent_group_name)
      .def(py::self == py::self)
      .def(py::self != py::self);

  py::class_<ParsedSrdfDisabledCollision>(
      module, "ParsedSrdfDisabledCollision", py::is_final())
      .def_readonly("first_link_name",
                    &ParsedSrdfDisabledCollision::first_link_name)
      .def_readonly("second_link_name",
                    &ParsedSrdfDisabledCollision::second_link_name)
      .def_readonly("reason",
                    &ParsedSrdfDisabledCollision::reason)
      .def(py::self == py::self)
      .def(py::self != py::self);

  py::class_<ParsedModelSourceIr>(
      module, "ParsedModelSourceIr", py::is_final())
      .def_readonly("robot_name", &ParsedModelSourceIr::robot_name)
      .def_readonly("root_link_name",
                    &ParsedModelSourceIr::root_link_name)
      .def_property_readonly(
          "link_names",
          [](const ParsedModelSourceIr& source) {
            return immutable_snapshot(source.link_names);
          })
      .def_property_readonly(
          "material_names",
          [](const ParsedModelSourceIr& source) {
            return immutable_snapshot(source.material_names);
          })
      .def_property_readonly(
          "joints",
          [](const ParsedModelSourceIr& source) {
            return immutable_snapshot(source.joints);
          })
      .def_property_readonly(
          "groups",
          [](const ParsedModelSourceIr& source) {
            return immutable_snapshot(source.groups);
          })
      .def_property_readonly(
          "group_states",
          [](const ParsedModelSourceIr& source) {
            return immutable_snapshot(source.group_states);
          })
      .def_property_readonly(
          "end_effectors",
          [](const ParsedModelSourceIr& source) {
            return immutable_snapshot(source.end_effectors);
          })
      .def_property_readonly(
          "passive_joint_names",
          [](const ParsedModelSourceIr& source) {
            return immutable_snapshot(source.passive_joint_names);
          })
      .def_property_readonly(
          "disabled_collisions",
          [](const ParsedModelSourceIr& source) {
            return immutable_snapshot(source.disabled_collisions);
          })
      .def_property_readonly(
          "referenced_artifact_logical_uris",
          [](const ParsedModelSourceIr& source) {
            return immutable_snapshot(
                source.referenced_artifact_logical_uris);
          })
      .def(py::self == py::self)
      .def(py::self != py::self);

  py::class_<SourceVerifiedModelAsset>(
      module, "SourceVerifiedModelAsset", py::is_final())
      .def_property_readonly("value",
                             &SourceVerifiedModelAsset::value)
      .def_property_readonly(
          "qualified_asset",
          &SourceVerifiedModelAsset::qualified_asset)
      .def_property_readonly(
          "verified_artifacts",
          [](const SourceVerifiedModelAsset& asset) {
            return immutable_snapshot(asset.verified_artifacts());
          })
      .def_property_readonly("parsed_ir",
                             &SourceVerifiedModelAsset::parsed_ir);

  py::class_<StructuralModelInstanceResolution>(
      module, "StructuralModelInstanceResolution", py::is_final())
      .def_readonly(
          "model_instance_id",
          &StructuralModelInstanceResolution::model_instance_id)
      .def_readonly("model_asset_id",
                    &StructuralModelInstanceResolution::model_asset_id)
      .def_readonly("model_revision",
                    &StructuralModelInstanceResolution::model_revision)
      .def_readonly(
          "parsed_root_link_name",
          &StructuralModelInstanceResolution::parsed_root_link_name)
      .def_readonly(
          "owner_component_id",
          &StructuralModelInstanceResolution::owner_component_id)
      .def_readonly(
          "owner_model_role_id",
          &StructuralModelInstanceResolution::owner_model_role_id)
      .def_readonly("placement_source",
                    &StructuralModelInstanceResolution::placement_source)
      .def(py::self == py::self)
      .def(py::self != py::self);

  py::class_<StructuralModelOccurrence>(
      module, "StructuralModelOccurrence", py::is_final())
      .def_readonly("model_role_id",
                    &StructuralModelOccurrence::model_role_id)
      .def_readonly("model_instance_id",
                    &StructuralModelOccurrence::model_instance_id)
      .def_readonly("model_asset_id",
                    &StructuralModelOccurrence::model_asset_id)
      .def_readonly("model_revision",
                    &StructuralModelOccurrence::model_revision)
      .def(py::self == py::self)
      .def(py::self != py::self);

  py::class_<StructuralFrameResolution>(
      module, "StructuralFrameResolution", py::is_final())
      .def_readonly("model", &StructuralFrameResolution::model)
      .def_readonly("frame_role_id",
                    &StructuralFrameResolution::frame_role_id)
      .def_readonly("source_link_name",
                    &StructuralFrameResolution::source_link_name)
      .def(py::self == py::self)
      .def(py::self != py::self);

  py::class_<StructuralModelMountResolution>(
      module, "StructuralModelMountResolution", py::is_final())
      .def_readonly("parent",
                    &StructuralModelMountResolution::parent)
      .def_readonly("child", &StructuralModelMountResolution::child)
      .def_readonly(
          "child_frame_pose_in_parent_frame",
          &StructuralModelMountResolution::
              child_frame_pose_in_parent_frame)
      .def(py::self == py::self)
      .def(py::self != py::self);

  py::class_<StructuralModelGroupResolution>(
      module, "StructuralModelGroupResolution", py::is_final())
      .def_readonly(
          "model_group_role_id",
          &StructuralModelGroupResolution::model_group_role_id)
      .def_readonly("model", &StructuralModelGroupResolution::model)
      .def_readonly(
          "asset_model_group_role_id",
          &StructuralModelGroupResolution::asset_model_group_role_id)
      .def_readonly("source_group_name",
                    &StructuralModelGroupResolution::source_group_name)
      .def(py::self == py::self)
      .def(py::self != py::self);

  py::class_<StructuralModelResourceResolution>(
      module, "StructuralModelResourceResolution", py::is_final())
      .def_readonly("resource_role_id",
                    &StructuralModelResourceResolution::resource_role_id)
      .def_readonly("model",
                    &StructuralModelResourceResolution::model)
      .def_readonly(
          "asset_resource_role_id",
          &StructuralModelResourceResolution::asset_resource_role_id)
      .def_property_readonly(
          "source_joint_names",
          [](const StructuralModelResourceResolution& resource) {
            return immutable_snapshot(resource.source_joint_names);
          })
      .def(py::self == py::self)
      .def(py::self != py::self);

  py::class_<StructuralComponentLocalResourceResolution>(
      module, "StructuralComponentLocalResourceResolution",
      py::is_final())
      .def_readonly(
          "resource_role_id",
          &StructuralComponentLocalResourceResolution::resource_role_id)
      .def(py::self == py::self)
      .def(py::self != py::self);

  py::tuple structural_resource_types{2U};
  structural_resource_types[0] =
      module.attr("StructuralModelResourceResolution");
  structural_resource_types[1] =
      module.attr("StructuralComponentLocalResourceResolution");
  module.attr("StructuralResourceResolution") =
      py::module_::import("typing")
          .attr("Union")
          .attr("__getitem__")(structural_resource_types);

  py::class_<StructuralTargetFrameResolution>(
      module, "StructuralTargetFrameResolution", py::is_final())
      .def_readonly("frame_role_id",
                    &StructuralTargetFrameResolution::frame_role_id)
      .def_readonly("source",
                    &StructuralTargetFrameResolution::source)
      .def(py::self == py::self)
      .def(py::self != py::self);

  py::class_<StructuralCollisionBodyResolution>(
      module, "StructuralCollisionBodyResolution", py::is_final())
      .def_readonly(
          "collision_body_role_id",
          &StructuralCollisionBodyResolution::collision_body_role_id)
      .def_readonly("model",
                    &StructuralCollisionBodyResolution::model)
      .def_readonly(
          "asset_collision_body_role_id",
          &StructuralCollisionBodyResolution::
              asset_collision_body_role_id)
      .def_property_readonly(
          "source_link_names",
          [](const StructuralCollisionBodyResolution& body) {
            return immutable_snapshot(body.source_link_names);
          })
      .def(py::self == py::self)
      .def(py::self != py::self);

  py::class_<StructuralCommandTargetResolution>(
      module, "StructuralCommandTargetResolution", py::is_final())
      .def_readonly(
          "command_target_id",
          &StructuralCommandTargetResolution::command_target_id)
      .def_property_readonly(
          "model_groups",
          [](const StructuralCommandTargetResolution& target) {
            return immutable_snapshot(target.model_groups);
          })
      .def_property_readonly(
          "resources",
          [](const StructuralCommandTargetResolution& target) {
            return immutable_snapshot(target.resources);
          })
      .def_property_readonly(
          "frames",
          [](const StructuralCommandTargetResolution& target) {
            return immutable_snapshot(target.frames);
          })
      .def_property_readonly(
          "collision_bodies",
          [](const StructuralCommandTargetResolution& target) {
            return immutable_snapshot(target.collision_bodies);
          })
      .def(py::self == py::self)
      .def(py::self != py::self);

  py::class_<StructuralAttachmentInterfaceResolution>(
      module, "StructuralAttachmentInterfaceResolution",
      py::is_final())
      .def_readonly(
          "attachment_interface_id",
          &StructuralAttachmentInterfaceResolution::
              attachment_interface_id)
      .def_readonly("frame",
                    &StructuralAttachmentInterfaceResolution::frame)
      .def_property_readonly(
          "accepts_tags",
          [](const StructuralAttachmentInterfaceResolution& interface) {
            return immutable_snapshot(interface.accepts_tags);
          })
      .def_readonly(
          "max_attachments",
          &StructuralAttachmentInterfaceResolution::max_attachments)
      .def(py::self == py::self)
      .def(py::self != py::self);

  py::class_<StructuralComponentResolution>(
      module, "StructuralComponentResolution", py::is_final())
      .def_readonly(
          "controllable_component_id",
          &StructuralComponentResolution::controllable_component_id)
      .def_readonly("component_type_id",
                    &StructuralComponentResolution::component_type_id)
      .def_readonly(
          "component_type_revision",
          &StructuralComponentResolution::component_type_revision)
      .def_property_readonly(
          "component_tags",
          [](const StructuralComponentResolution& component) {
            return immutable_snapshot(component.component_tags);
          })
      .def_property_readonly(
          "model_roles",
          [](const StructuralComponentResolution& component) {
            return immutable_snapshot(component.model_roles);
          })
      .def_readonly("component_root",
                    &StructuralComponentResolution::component_root)
      .def_property_readonly(
          "model_mounts",
          [](const StructuralComponentResolution& component) {
            return immutable_snapshot(component.model_mounts);
          })
      .def_property_readonly(
          "command_targets",
          [](const StructuralComponentResolution& component) {
            return immutable_snapshot(component.command_targets);
          })
      .def_property_readonly(
          "attachment_interfaces",
          [](const StructuralComponentResolution& component) {
            return immutable_snapshot(component.attachment_interfaces);
          })
      .def(py::self == py::self)
      .def(py::self != py::self);

  py::class_<StructuralAttachmentEndpointResolution>(
      module, "StructuralAttachmentEndpointResolution",
      py::is_final())
      .def_readonly(
          "controllable_component_id",
          &StructuralAttachmentEndpointResolution::
              controllable_component_id)
      .def_readonly(
          "attachment_interface_id",
          &StructuralAttachmentEndpointResolution::
              attachment_interface_id)
      .def_readonly("frame",
                    &StructuralAttachmentEndpointResolution::frame)
      .def(py::self == py::self)
      .def(py::self != py::self);

  py::class_<StructuralAttachmentResolution>(
      module, "StructuralAttachmentResolution", py::is_final())
      .def_readonly(
          "component_attachment_id",
          &StructuralAttachmentResolution::component_attachment_id)
      .def_readonly("parent",
                    &StructuralAttachmentResolution::parent)
      .def_readonly("child", &StructuralAttachmentResolution::child)
      .def_readonly(
          "child_interface_pose_in_parent_interface",
          &StructuralAttachmentResolution::
              child_interface_pose_in_parent_interface)
      .def(py::self == py::self)
      .def(py::self != py::self);

  py::class_<StructurallyQualifiedCellGraph>(
      module, "StructurallyQualifiedCellGraph", py::is_final())
      .def_property_readonly(
          "verified_graph",
          &StructurallyQualifiedCellGraph::verified_graph)
      .def_property_readonly("graph",
                             &StructurallyQualifiedCellGraph::graph)
      .def_property_readonly(
          "model_assets",
          [](const StructurallyQualifiedCellGraph& graph) {
            return immutable_snapshot(graph.model_assets());
          })
      .def_property_readonly(
          "component_types",
          [](const StructurallyQualifiedCellGraph& graph) {
            return immutable_snapshot(graph.component_types());
          })
      .def_property_readonly(
          "model_instances",
          [](const StructurallyQualifiedCellGraph& graph) {
            return immutable_snapshot(graph.model_instances());
          })
      .def_property_readonly(
          "components",
          [](const StructurallyQualifiedCellGraph& graph) {
            return immutable_snapshot(graph.components());
          })
      .def_property_readonly(
          "attachments",
          [](const StructurallyQualifiedCellGraph& graph) {
            return immutable_snapshot(graph.attachments());
          });

  module.def(
      "preflight_model_source_declaration",
      &deployment::preflight_model_source_declaration,
      py::arg("qualified_asset"),
      py::call_guard<py::gil_scoped_release>());

  module.def(
      "verify_model_source_closure",
      [](const ModelSourceRetrievalPlan& retrieval_plan,
         const py::iterable& resolved_artifacts) {
        auto bounded = bounded_source_closure_from_python(
            retrieval_plan, resolved_artifacts);
        py::gil_scoped_release release;
        return deployment::verify_model_source_closure(
            retrieval_plan, std::move(bounded));
      },
      py::arg("retrieval_plan"), py::arg("resolved_artifacts"));

  module.def(
      "verify_model_source_closure",
      [](const deployment::QualifiedModelAsset& qualified_asset,
         const py::iterable& resolved_artifacts) {
        const auto retrieval_plan = [&qualified_asset]() {
          py::gil_scoped_release release;
          return deployment::preflight_model_source_declaration(
              qualified_asset);
        }();
        auto bounded = bounded_source_closure_from_python(
            retrieval_plan, resolved_artifacts);
        py::gil_scoped_release release;
        return deployment::verify_model_source_closure(
            retrieval_plan, std::move(bounded));
      },
      py::arg("qualified_asset"), py::arg("resolved_artifacts"));

  module.def(
      "qualify_cell_structure",
      [](const deployment::GraphRevisionVerifiedCellGraph& graph,
         const py::iterable& model_assets,
         const py::iterable& component_types) {
        std::set<ExactRevisionKey> selected_assets;
        std::set<ExactRevisionKey> selected_types;
        {
          py::gil_scoped_release release;
          selected_assets = selected_model_assets(graph);
          selected_types = selected_component_types(graph);
        }
        auto bounded_assets =
            bounded_exact_closure_from_python<
                deployment::SourceVerifiedModelAsset>(
                model_assets, selected_assets, "model_assets",
                "SourceVerifiedModelAsset",
                [](const deployment::SourceVerifiedModelAsset& asset) {
                  return ExactRevisionKey{
                      asset.value().model_asset_id.value(),
                      asset.value().model_revision.value()};
                });
        auto bounded_types =
            bounded_exact_closure_from_python<
                deployment::QualifiedComponentType>(
                component_types, selected_types, "component_types",
                "QualifiedComponentType",
                [](const deployment::QualifiedComponentType& type) {
                  return ExactRevisionKey{
                      type.value().component_type_id.value(),
                      type.value().component_type_revision.value()};
                });
        py::gil_scoped_release release;
        return deployment::qualify_cell_structure(
            graph,
            std::span<const deployment::SourceVerifiedModelAsset>{
                bounded_assets},
            std::span<const deployment::QualifiedComponentType>{
                bounded_types});
      },
      py::arg("graph"), py::arg("model_assets"),
      py::arg("component_types"));

  module.attr("MAX_MODEL_SOURCE_XML_ENTRYPOINT_BYTES") =
      py::int_(deployment::kMaxModelSourceXmlEntrypointBytes);
  module.attr("MAX_MODEL_SOURCE_OPAQUE_ARTIFACT_BYTES") =
      py::int_(deployment::kMaxModelSourceOpaqueArtifactBytes);
  module.attr("MAX_MODEL_SOURCE_CLOSURE_BYTES") =
      py::int_(deployment::kMaxModelSourceClosureBytes);
  module.attr("MAX_MODEL_SOURCE_XML_DEPTH") =
      py::int_(deployment::kMaxModelSourceXmlDepth);
  module.attr("MAX_MODEL_SOURCE_XML_ELEMENT_COUNT") =
      py::int_(deployment::kMaxModelSourceXmlElementCount);
  module.attr("MAX_MODEL_SOURCE_XML_ATTRIBUTE_COUNT") =
      py::int_(deployment::kMaxModelSourceXmlAttributeCount);
}

}  // namespace manyforge::core::python
