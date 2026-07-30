#include <pybind11/operators.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
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

template <typename Values>
[[nodiscard]] py::object optional_immutable_snapshot(
    const std::optional<Values>& values) {
  if (!values.has_value()) {
    return py::none();
  }
  return immutable_snapshot(*values);
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
  using deployment::ExpandedSrdfGroupIr;
  using deployment::ModelSourceGroupStateIndexEntryIr;
  using deployment::ModelSourceIr;
  using deployment::ModelSourceNameIndexEntryIr;
  using deployment::ModelSourceRetrievalPlan;
  using deployment::ModelSourceRetrievalPlanRow;
  using deployment::ModelSourceSemanticIndex;
  using deployment::ResolvedModelSourceArtifactCandidate;
  using deployment::SourceArtifactReferenceIr;
  using deployment::SourceVerifiedModelAsset;
  using deployment::SrdfChainGroupMemberIr;
  using deployment::SrdfDisabledCollisionIr;
  using deployment::SrdfDocumentIr;
  using deployment::SrdfEndEffectorIr;
  using deployment::SrdfExpandedMemberIr;
  using deployment::SrdfExpansionProvenanceNodeIr;
  using deployment::SrdfGroupIr;
  using deployment::SrdfGroupStateIr;
  using deployment::SrdfGroupStateJointIr;
  using deployment::SrdfJointGroupMemberIr;
  using deployment::SrdfLinkGroupMemberIr;
  using deployment::SrdfSubgroupMemberIr;
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
  using deployment::UrdfAxisIr;
  using deployment::UrdfBoxGeometryIr;
  using deployment::UrdfCalibrationIr;
  using deployment::UrdfCollisionIr;
  using deployment::UrdfCylinderGeometryIr;
  using deployment::UrdfDocumentIr;
  using deployment::UrdfDynamicsIr;
  using deployment::UrdfInertiaIr;
  using deployment::UrdfInertialIr;
  using deployment::UrdfJointIr;
  using deployment::UrdfLimitIr;
  using deployment::UrdfLinkIr;
  using deployment::UrdfMaterialColorIr;
  using deployment::UrdfMaterialIr;
  using deployment::UrdfMaterialTextureIr;
  using deployment::UrdfResolvedMaterialReferenceIr;
  using deployment::UrdfMeshGeometryIr;
  using deployment::UrdfMimicIr;
  using deployment::UrdfOriginIr;
  using deployment::UrdfSafetyControllerIr;
  using deployment::UrdfSphereGeometryIr;
  using deployment::UrdfVisualIr;
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
      .value("kColladaMesh",
             deployment::ModelSourceArtifactKind::kColladaMesh)
      .value("kPngTexture",
             deployment::ModelSourceArtifactKind::kPngTexture)
      .value("kJpegTexture",
             deployment::ModelSourceArtifactKind::kJpegTexture);

  py::enum_<deployment::UrdfJointType>(module, "UrdfJointType")
      .value("kRevolute", deployment::UrdfJointType::kRevolute)
      .value("kContinuous", deployment::UrdfJointType::kContinuous)
      .value("kPrismatic", deployment::UrdfJointType::kPrismatic)
      .value("kFixed", deployment::UrdfJointType::kFixed)
      .value("kFloating", deployment::UrdfJointType::kFloating)
      .value("kPlanar", deployment::UrdfJointType::kPlanar);

  py::enum_<deployment::UrdfCoordinateKind>(
      module, "UrdfCoordinateKind")
      .value("kNone", deployment::UrdfCoordinateKind::kNone)
      .value("kAngular", deployment::UrdfCoordinateKind::kAngular)
      .value("kLinear", deployment::UrdfCoordinateKind::kLinear)
      .value("kPlanar", deployment::UrdfCoordinateKind::kPlanar)
      .value("kFloating", deployment::UrdfCoordinateKind::kFloating);

  py::enum_<deployment::SourceArtifactReferenceKind>(
      module, "SourceArtifactReferenceKind")
      .value("kMesh",
             deployment::SourceArtifactReferenceKind::kMesh)
      .value("kTexture",
             deployment::SourceArtifactReferenceKind::kTexture);

  py::enum_<deployment::SrdfExpansionProvenanceKind>(
      module, "SrdfExpansionProvenanceKind")
      .value("kDirectLink",
             deployment::SrdfExpansionProvenanceKind::kDirectLink)
      .value("kDirectJoint",
             deployment::SrdfExpansionProvenanceKind::kDirectJoint)
      .value("kChainLink",
             deployment::SrdfExpansionProvenanceKind::kChainLink)
      .value("kChainJoint",
             deployment::SrdfExpansionProvenanceKind::kChainJoint)
      .value("kSubgroup",
             deployment::SrdfExpansionProvenanceKind::kSubgroup);

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

  py::class_<UrdfOriginIr>(module, "UrdfOriginIr", py::is_final())
      .def_readonly("element_authored", &UrdfOriginIr::element_authored)
      .def_property_readonly(
          "authored_xyz_m",
          [](const UrdfOriginIr& value) {
            return optional_immutable_snapshot(value.authored_xyz_m);
          })
      .def_property_readonly(
          "authored_rpy_rad",
          [](const UrdfOriginIr& value) {
            return optional_immutable_snapshot(value.authored_rpy_rad);
          })
      .def_property_readonly(
          "xyz_m",
          [](const UrdfOriginIr& value) {
            return immutable_snapshot(value.xyz_m);
          })
      .def_property_readonly(
          "rpy_rad",
          [](const UrdfOriginIr& value) {
            return immutable_snapshot(value.rpy_rad);
          })
      .def(py::self == py::self)
      .def(py::self != py::self);

  py::class_<UrdfBoxGeometryIr>(
      module, "UrdfBoxGeometryIr", py::is_final())
      .def_property_readonly(
          "size_m",
          [](const UrdfBoxGeometryIr& value) {
            return immutable_snapshot(value.size_m);
          })
      .def(py::self == py::self)
      .def(py::self != py::self);
  py::class_<UrdfCylinderGeometryIr>(
      module, "UrdfCylinderGeometryIr", py::is_final())
      .def_readonly("radius_m", &UrdfCylinderGeometryIr::radius_m)
      .def_readonly("length_m", &UrdfCylinderGeometryIr::length_m)
      .def(py::self == py::self)
      .def(py::self != py::self);
  py::class_<UrdfSphereGeometryIr>(
      module, "UrdfSphereGeometryIr", py::is_final())
      .def_readonly("radius_m", &UrdfSphereGeometryIr::radius_m)
      .def(py::self == py::self)
      .def(py::self != py::self);
  py::class_<UrdfMeshGeometryIr>(
      module, "UrdfMeshGeometryIr", py::is_final())
      .def_readonly("logical_uri", &UrdfMeshGeometryIr::logical_uri)
      .def_property_readonly(
          "authored_scale",
          [](const UrdfMeshGeometryIr& value) {
            return optional_immutable_snapshot(value.authored_scale);
          })
      .def_property_readonly(
          "scale",
          [](const UrdfMeshGeometryIr& value) {
            return immutable_snapshot(value.scale);
          })
      .def(py::self == py::self)
      .def(py::self != py::self);

  py::tuple urdf_geometry_types{4U};
  urdf_geometry_types[0] = module.attr("UrdfBoxGeometryIr");
  urdf_geometry_types[1] = module.attr("UrdfCylinderGeometryIr");
  urdf_geometry_types[2] = module.attr("UrdfSphereGeometryIr");
  urdf_geometry_types[3] = module.attr("UrdfMeshGeometryIr");
  module.attr("UrdfGeometryIr") =
      py::module_::import("typing")
          .attr("Union")
          .attr("__getitem__")(urdf_geometry_types);

  py::class_<UrdfMaterialColorIr>(
      module, "UrdfMaterialColorIr", py::is_final())
      .def_property_readonly(
          "rgba",
          [](const UrdfMaterialColorIr& value) {
            return immutable_snapshot(value.rgba);
          })
      .def(py::self == py::self)
      .def(py::self != py::self);
  py::class_<UrdfMaterialTextureIr>(
      module, "UrdfMaterialTextureIr", py::is_final())
      .def_readonly("logical_uri",
                    &UrdfMaterialTextureIr::logical_uri)
      .def(py::self == py::self)
      .def(py::self != py::self);
  py::class_<UrdfResolvedMaterialReferenceIr>(
      module, "UrdfResolvedMaterialReferenceIr", py::is_final())
      .def_readonly("definition_name",
                    &UrdfResolvedMaterialReferenceIr::definition_name)
      .def_readonly("color",
                    &UrdfResolvedMaterialReferenceIr::color)
      .def_readonly("texture",
                    &UrdfResolvedMaterialReferenceIr::texture)
      .def(py::self == py::self)
      .def(py::self != py::self);
  py::class_<UrdfMaterialIr>(module, "UrdfMaterialIr", py::is_final())
      .def_readonly("name", &UrdfMaterialIr::name)
      .def_readonly("color", &UrdfMaterialIr::color)
      .def_readonly("texture", &UrdfMaterialIr::texture)
      .def_readonly("resolved_reference",
                    &UrdfMaterialIr::resolved_reference)
      .def(py::self == py::self)
      .def(py::self != py::self);

  py::class_<UrdfVisualIr>(module, "UrdfVisualIr", py::is_final())
      .def_readonly("name", &UrdfVisualIr::name)
      .def_readonly("origin", &UrdfVisualIr::origin)
      .def_readonly("geometry", &UrdfVisualIr::geometry)
      .def_readonly("material", &UrdfVisualIr::material)
      .def(py::self == py::self)
      .def(py::self != py::self);
  py::class_<UrdfCollisionIr>(
      module, "UrdfCollisionIr", py::is_final())
      .def_readonly("name", &UrdfCollisionIr::name)
      .def_readonly("origin", &UrdfCollisionIr::origin)
      .def_readonly("geometry", &UrdfCollisionIr::geometry)
      .def(py::self == py::self)
      .def(py::self != py::self);
  py::class_<UrdfInertiaIr>(module, "UrdfInertiaIr", py::is_final())
      .def_readonly("ixx", &UrdfInertiaIr::ixx)
      .def_readonly("ixy", &UrdfInertiaIr::ixy)
      .def_readonly("ixz", &UrdfInertiaIr::ixz)
      .def_readonly("iyy", &UrdfInertiaIr::iyy)
      .def_readonly("iyz", &UrdfInertiaIr::iyz)
      .def_readonly("izz", &UrdfInertiaIr::izz)
      .def(py::self == py::self)
      .def(py::self != py::self);
  py::class_<UrdfInertialIr>(module, "UrdfInertialIr", py::is_final())
      .def_readonly("origin", &UrdfInertialIr::origin)
      .def_readonly("mass_kg", &UrdfInertialIr::mass_kg)
      .def_readonly("inertia", &UrdfInertialIr::inertia)
      .def(py::self == py::self)
      .def(py::self != py::self);
  py::class_<UrdfLinkIr>(module, "UrdfLinkIr", py::is_final())
      .def_readonly("name", &UrdfLinkIr::name)
      .def_readonly("inertial", &UrdfLinkIr::inertial)
      .def_property_readonly(
          "visuals",
          [](const UrdfLinkIr& value) {
            return immutable_snapshot(value.visuals);
          })
      .def_property_readonly(
          "collisions",
          [](const UrdfLinkIr& value) {
            return immutable_snapshot(value.collisions);
          })
      .def(py::self == py::self)
      .def(py::self != py::self);

  py::class_<UrdfAxisIr>(module, "UrdfAxisIr", py::is_final())
      .def_readonly("element_authored", &UrdfAxisIr::element_authored)
      .def_property_readonly(
          "authored_xyz",
          [](const UrdfAxisIr& value) {
            return optional_immutable_snapshot(value.authored_xyz);
          })
      .def_property_readonly(
          "xyz",
          [](const UrdfAxisIr& value) {
            return immutable_snapshot(value.xyz);
          })
      .def(py::self == py::self)
      .def(py::self != py::self);
  py::class_<UrdfCalibrationIr>(
      module, "UrdfCalibrationIr", py::is_final())
      .def_readonly("rising", &UrdfCalibrationIr::rising)
      .def_readonly("falling", &UrdfCalibrationIr::falling)
      .def_readonly("authored_reference_position",
                    &UrdfCalibrationIr::authored_reference_position)
      .def_readonly("reference_position",
                    &UrdfCalibrationIr::reference_position)
      .def(py::self == py::self)
      .def(py::self != py::self);
  py::class_<UrdfDynamicsIr>(module, "UrdfDynamicsIr", py::is_final())
      .def_readonly("authored_damping",
                    &UrdfDynamicsIr::authored_damping)
      .def_readonly("authored_friction",
                    &UrdfDynamicsIr::authored_friction)
      .def_readonly("damping", &UrdfDynamicsIr::damping)
      .def_readonly("friction", &UrdfDynamicsIr::friction)
      .def(py::self == py::self)
      .def(py::self != py::self);
  py::class_<UrdfLimitIr>(module, "UrdfLimitIr", py::is_final())
      .def_readonly("authored_lower", &UrdfLimitIr::authored_lower)
      .def_readonly("authored_upper", &UrdfLimitIr::authored_upper)
      .def_readonly("lower", &UrdfLimitIr::lower)
      .def_readonly("upper", &UrdfLimitIr::upper)
      .def_readonly("effort", &UrdfLimitIr::effort)
      .def_readonly("velocity", &UrdfLimitIr::velocity)
      .def(py::self == py::self)
      .def(py::self != py::self);
  py::class_<UrdfMimicIr>(module, "UrdfMimicIr", py::is_final())
      .def_readonly("joint_name", &UrdfMimicIr::joint_name)
      .def_readonly("authored_multiplier",
                    &UrdfMimicIr::authored_multiplier)
      .def_readonly("authored_offset", &UrdfMimicIr::authored_offset)
      .def_readonly("multiplier", &UrdfMimicIr::multiplier)
      .def_readonly("offset", &UrdfMimicIr::offset)
      .def(py::self == py::self)
      .def(py::self != py::self);
  py::class_<UrdfSafetyControllerIr>(
      module, "UrdfSafetyControllerIr", py::is_final())
      .def_readonly("authored_soft_lower_limit",
                    &UrdfSafetyControllerIr::authored_soft_lower_limit)
      .def_readonly("authored_soft_upper_limit",
                    &UrdfSafetyControllerIr::authored_soft_upper_limit)
      .def_readonly("authored_k_position",
                    &UrdfSafetyControllerIr::authored_k_position)
      .def_readonly("soft_lower_limit",
                    &UrdfSafetyControllerIr::soft_lower_limit)
      .def_readonly("soft_upper_limit",
                    &UrdfSafetyControllerIr::soft_upper_limit)
      .def_readonly("k_position", &UrdfSafetyControllerIr::k_position)
      .def_readonly("k_velocity", &UrdfSafetyControllerIr::k_velocity)
      .def(py::self == py::self)
      .def(py::self != py::self);
  py::class_<UrdfJointIr>(module, "UrdfJointIr", py::is_final())
      .def_readonly("name", &UrdfJointIr::name)
      .def_readonly("type", &UrdfJointIr::type)
      .def_readonly("coordinate_kind", &UrdfJointIr::coordinate_kind)
      .def_readonly("parent_link_name", &UrdfJointIr::parent_link_name)
      .def_readonly("child_link_name", &UrdfJointIr::child_link_name)
      .def_readonly("origin", &UrdfJointIr::origin)
      .def_readonly("axis", &UrdfJointIr::axis)
      .def_readonly("calibration", &UrdfJointIr::calibration)
      .def_readonly("dynamics", &UrdfJointIr::dynamics)
      .def_readonly("limit", &UrdfJointIr::limit)
      .def_readonly("mimic", &UrdfJointIr::mimic)
      .def_readonly("safety_controller",
                    &UrdfJointIr::safety_controller)
      .def(py::self == py::self)
      .def(py::self != py::self);
  py::class_<UrdfDocumentIr>(module, "UrdfDocumentIr", py::is_final())
      .def_readonly("robot_name", &UrdfDocumentIr::robot_name)
      .def_property_readonly(
          "materials",
          [](const UrdfDocumentIr& value) {
            return immutable_snapshot(value.materials);
          })
      .def_property_readonly(
          "links",
          [](const UrdfDocumentIr& value) {
            return immutable_snapshot(value.links);
          })
      .def_property_readonly(
          "joints",
          [](const UrdfDocumentIr& value) {
            return immutable_snapshot(value.joints);
          })
      .def(py::self == py::self)
      .def(py::self != py::self);

  py::class_<SrdfLinkGroupMemberIr>(
      module, "SrdfLinkGroupMemberIr", py::is_final())
      .def_readonly("link_name", &SrdfLinkGroupMemberIr::link_name)
      .def(py::self == py::self)
      .def(py::self != py::self);
  py::class_<SrdfJointGroupMemberIr>(
      module, "SrdfJointGroupMemberIr", py::is_final())
      .def_readonly("joint_name", &SrdfJointGroupMemberIr::joint_name)
      .def(py::self == py::self)
      .def(py::self != py::self);
  py::class_<SrdfChainGroupMemberIr>(
      module, "SrdfChainGroupMemberIr", py::is_final())
      .def_readonly("base_link_name",
                    &SrdfChainGroupMemberIr::base_link_name)
      .def_readonly("tip_link_name",
                    &SrdfChainGroupMemberIr::tip_link_name)
      .def(py::self == py::self)
      .def(py::self != py::self);
  py::class_<SrdfSubgroupMemberIr>(
      module, "SrdfSubgroupMemberIr", py::is_final())
      .def_readonly("group_name", &SrdfSubgroupMemberIr::group_name)
      .def(py::self == py::self)
      .def(py::self != py::self);
  py::tuple srdf_group_member_types{4U};
  srdf_group_member_types[0] = module.attr("SrdfLinkGroupMemberIr");
  srdf_group_member_types[1] = module.attr("SrdfJointGroupMemberIr");
  srdf_group_member_types[2] = module.attr("SrdfChainGroupMemberIr");
  srdf_group_member_types[3] = module.attr("SrdfSubgroupMemberIr");
  module.attr("SrdfGroupMemberIr") =
      py::module_::import("typing")
          .attr("Union")
          .attr("__getitem__")(srdf_group_member_types);
  py::class_<SrdfGroupIr>(module, "SrdfGroupIr", py::is_final())
      .def_readonly("name", &SrdfGroupIr::name)
      .def_property_readonly(
          "members",
          [](const SrdfGroupIr& value) {
            return immutable_snapshot(value.members);
          })
      .def(py::self == py::self)
      .def(py::self != py::self);
  py::class_<SrdfGroupStateJointIr>(
      module, "SrdfGroupStateJointIr", py::is_final())
      .def_readonly("joint_name", &SrdfGroupStateJointIr::joint_name)
      .def_readonly("value", &SrdfGroupStateJointIr::value)
      .def(py::self == py::self)
      .def(py::self != py::self);
  py::class_<SrdfGroupStateIr>(
      module, "SrdfGroupStateIr", py::is_final())
      .def_readonly("name", &SrdfGroupStateIr::name)
      .def_readonly("group_name", &SrdfGroupStateIr::group_name)
      .def_property_readonly(
          "joints",
          [](const SrdfGroupStateIr& value) {
            return immutable_snapshot(value.joints);
          })
      .def(py::self == py::self)
      .def(py::self != py::self);
  py::class_<SrdfEndEffectorIr>(
      module, "SrdfEndEffectorIr", py::is_final())
      .def_readonly("name", &SrdfEndEffectorIr::name)
      .def_readonly("parent_link_name",
                    &SrdfEndEffectorIr::parent_link_name)
      .def_readonly("group_name", &SrdfEndEffectorIr::group_name)
      .def_readonly("parent_group_name",
                    &SrdfEndEffectorIr::parent_group_name)
      .def(py::self == py::self)
      .def(py::self != py::self);
  py::class_<SrdfDisabledCollisionIr>(
      module, "SrdfDisabledCollisionIr", py::is_final())
      .def_readonly("first_link_name",
                    &SrdfDisabledCollisionIr::first_link_name)
      .def_readonly("second_link_name",
                    &SrdfDisabledCollisionIr::second_link_name)
      .def_readonly("reason", &SrdfDisabledCollisionIr::reason)
      .def(py::self == py::self)
      .def(py::self != py::self);
  py::class_<SrdfDocumentIr>(module, "SrdfDocumentIr", py::is_final())
      .def_readonly("robot_name", &SrdfDocumentIr::robot_name)
      .def_property_readonly(
          "groups",
          [](const SrdfDocumentIr& value) {
            return immutable_snapshot(value.groups);
          })
      .def_property_readonly(
          "group_states",
          [](const SrdfDocumentIr& value) {
            return immutable_snapshot(value.group_states);
          })
      .def_property_readonly(
          "end_effectors",
          [](const SrdfDocumentIr& value) {
            return immutable_snapshot(value.end_effectors);
          })
      .def_property_readonly(
          "passive_joint_names",
          [](const SrdfDocumentIr& value) {
            return immutable_snapshot(value.passive_joint_names);
          })
      .def_property_readonly(
          "disabled_collisions",
          [](const SrdfDocumentIr& value) {
            return immutable_snapshot(value.disabled_collisions);
          })
      .def(py::self == py::self)
      .def(py::self != py::self);

  py::class_<SourceArtifactReferenceIr>(
      module, "SourceArtifactReferenceIr", py::is_final())
      .def_readonly("logical_uri",
                    &SourceArtifactReferenceIr::logical_uri)
      .def_readonly("kind", &SourceArtifactReferenceIr::kind)
      .def(py::self == py::self)
      .def(py::self != py::self);
  py::class_<ModelSourceNameIndexEntryIr>(
      module, "ModelSourceNameIndexEntryIr", py::is_final())
      .def_readonly("name", &ModelSourceNameIndexEntryIr::name)
      .def_readonly("source_index",
                    &ModelSourceNameIndexEntryIr::source_index)
      .def(py::self == py::self)
      .def(py::self != py::self);
  py::class_<ModelSourceGroupStateIndexEntryIr>(
      module, "ModelSourceGroupStateIndexEntryIr", py::is_final())
      .def_readonly("group_name",
                    &ModelSourceGroupStateIndexEntryIr::group_name)
      .def_readonly("state_name",
                    &ModelSourceGroupStateIndexEntryIr::state_name)
      .def_readonly("source_index",
                    &ModelSourceGroupStateIndexEntryIr::source_index)
      .def(py::self == py::self)
      .def(py::self != py::self);
  py::class_<SrdfExpansionProvenanceNodeIr>(
      module, "SrdfExpansionProvenanceNodeIr", py::is_final())
      .def_readonly("expansion_group_name",
                    &SrdfExpansionProvenanceNodeIr::expansion_group_name)
      .def_readonly("source_member_index",
                    &SrdfExpansionProvenanceNodeIr::source_member_index)
      .def_readonly("kind", &SrdfExpansionProvenanceNodeIr::kind)
      .def_readonly("source_symbol_name",
                    &SrdfExpansionProvenanceNodeIr::source_symbol_name)
      .def_readonly("chain_base_link_name",
                    &SrdfExpansionProvenanceNodeIr::chain_base_link_name)
      .def_readonly("chain_tip_link_name",
                    &SrdfExpansionProvenanceNodeIr::chain_tip_link_name)
      .def_readonly("nested_provenance_index",
                    &SrdfExpansionProvenanceNodeIr::
                        nested_provenance_index)
      .def(py::self == py::self)
      .def(py::self != py::self);
  py::class_<SrdfExpandedMemberIr>(
      module, "SrdfExpandedMemberIr", py::is_final())
      .def_readonly("name", &SrdfExpandedMemberIr::name)
      .def_property_readonly(
          "provenance_indices",
          [](const SrdfExpandedMemberIr& value) {
            return immutable_snapshot(value.provenance_indices);
          })
      .def(py::self == py::self)
      .def(py::self != py::self);
  py::class_<ExpandedSrdfGroupIr>(
      module, "ExpandedSrdfGroupIr", py::is_final())
      .def_readonly("group_name", &ExpandedSrdfGroupIr::group_name)
      .def_property_readonly(
          "links",
          [](const ExpandedSrdfGroupIr& value) {
            return immutable_snapshot(value.links);
          })
      .def_property_readonly(
          "joints",
          [](const ExpandedSrdfGroupIr& value) {
            return immutable_snapshot(value.joints);
          })
      .def(py::self == py::self)
      .def(py::self != py::self);
  py::class_<ModelSourceSemanticIndex>(
      module, "ModelSourceSemanticIndex", py::is_final())
      .def_readonly("root_link_name",
                    &ModelSourceSemanticIndex::root_link_name)
      .def_property_readonly(
          "material_names",
          [](const ModelSourceSemanticIndex& value) {
            return immutable_snapshot(value.material_names);
          })
      .def_property_readonly(
          "link_names",
          [](const ModelSourceSemanticIndex& value) {
            return immutable_snapshot(value.link_names);
          })
      .def_property_readonly(
          "joint_names",
          [](const ModelSourceSemanticIndex& value) {
            return immutable_snapshot(value.joint_names);
          })
      .def_property_readonly(
          "group_names",
          [](const ModelSourceSemanticIndex& value) {
            return immutable_snapshot(value.group_names);
          })
      .def_property_readonly(
          "group_state_names",
          [](const ModelSourceSemanticIndex& value) {
            return immutable_snapshot(value.group_state_names);
          })
      .def_property_readonly(
          "end_effector_names",
          [](const ModelSourceSemanticIndex& value) {
            return immutable_snapshot(value.end_effector_names);
          })
      .def_property_readonly(
          "group_expansion_postorder",
          [](const ModelSourceSemanticIndex& value) {
            return immutable_snapshot(value.group_expansion_postorder);
          })
      .def_property_readonly(
          "group_expansion_provenance",
          [](const ModelSourceSemanticIndex& value) {
            return immutable_snapshot(value.group_expansion_provenance);
          })
      .def_property_readonly(
          "expanded_groups",
          [](const ModelSourceSemanticIndex& value) {
            return immutable_snapshot(value.expanded_groups);
          })
      .def(py::self == py::self)
      .def(py::self != py::self);
  py::class_<ModelSourceIr>(module, "ModelSourceIr", py::is_final())
      .def_property_readonly("urdf", &ModelSourceIr::urdf)
      .def_property_readonly("srdf", &ModelSourceIr::srdf)
      .def_property_readonly("semantic_index",
                             &ModelSourceIr::semantic_index)
      .def_property_readonly(
          "artifact_references",
          [](const ModelSourceIr& value) {
            return immutable_snapshot(value.artifact_references());
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
      .def_property_readonly("source_ir",
                             &SourceVerifiedModelAsset::source_ir);

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
          "source_root_link_name",
          &StructuralModelInstanceResolution::source_root_link_name)
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

  module.def("emit_canonical_urdf",
             &deployment::emit_canonical_urdf,
             py::arg("source_ir"),
             py::call_guard<py::gil_scoped_release>());
  module.def("emit_canonical_srdf",
             &deployment::emit_canonical_srdf,
             py::arg("source_ir"),
             py::call_guard<py::gil_scoped_release>());

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
  module.attr("MAX_MODEL_SOURCE_COLLADA_XML_DEPTH") =
      py::int_(deployment::kMaxModelSourceColladaXmlDepth);
  module.attr("MAX_MODEL_SOURCE_COLLADA_XML_ELEMENT_COUNT") =
      py::int_(deployment::kMaxModelSourceColladaXmlElementCount);
  module.attr("MAX_MODEL_SOURCE_COLLADA_XML_ATTRIBUTE_COUNT") =
      py::int_(deployment::kMaxModelSourceColladaXmlAttributeCount);
  module.attr("MAX_MODEL_SOURCE_EXPANDED_SRDF_SEMANTIC_ROWS") =
      py::int_(
          deployment::kMaxModelSourceExpandedSrdfSemanticRows);
}

}  // namespace manyforge::core::python
