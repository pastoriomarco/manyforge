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
#include <manyforge_deployment/artifact_codec.hpp>
#include <manyforge_deployment/cell_applicability.hpp>
#include <manyforge_deployment/cell_compiler.hpp>
#include <manyforge_deployment/cell_composition.hpp>
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
    snapshot[index] =
        py::cast(values[index], py::return_value_policy::copy);
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
      throw deployment::CellCompositionError(
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
      throw deployment::CellCompositionError(
          std::string(selected_id ? "stale " : "unused extra ") +
          std::string(record_name) + " '" + key.first +
          "' at revision '" + key.second + "'");
    }
    if (!seen.insert(key).second) {
      throw deployment::CellCompositionError(
          "duplicate exact " + std::string(record_name) + " '" +
          key.first + "' at revision '" + key.second + "'");
    }
    bounded.push_back(qualified);
  }
  return bounded;
}

template <typename Qualified>
[[nodiscard]] std::vector<Qualified> bounded_qualified_closure_from_python(
    const py::iterable& values, std::string_view argument_name,
    std::string_view record_name) {
  std::vector<Qualified> bounded;
  for (const auto& item : values) {
    if (bounded.size() == deployment::kMaxArtifactNodeCount) {
      throw deployment::CellApplicabilityError(
          std::string(argument_name) + " exceeds the bounded closure limit");
    }
    if (!py::isinstance<Qualified>(item)) {
      throw py::type_error(std::string(argument_name) +
                           " must contain only " +
                           std::string(record_name) + " values");
    }
    bounded.push_back(item.cast<const Qualified&>());
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
  py::register_exception<deployment::CellCompositionError>(
      module, "CellCompositionError", PyExc_ValueError);
  py::register_exception<deployment::CellApplicabilityError>(
      module, "CellApplicabilityError", PyExc_ValueError);
  py::register_exception<deployment::CellCompilerError>(
      module, "CellCompilerError", PyExc_ValueError);

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

  py::enum_<deployment::OccurrenceSymbolKind>(module,
                                               "OccurrenceSymbolKind")
      .value("kModel", deployment::OccurrenceSymbolKind::kModel)
      .value("kRobot", deployment::OccurrenceSymbolKind::kRobot)
      .value("kMaterial", deployment::OccurrenceSymbolKind::kMaterial)
      .value("kLink", deployment::OccurrenceSymbolKind::kLink)
      .value("kJoint", deployment::OccurrenceSymbolKind::kJoint)
      .value("kVisual", deployment::OccurrenceSymbolKind::kVisual)
      .value("kCollision", deployment::OccurrenceSymbolKind::kCollision)
      .value("kSrdfGroup", deployment::OccurrenceSymbolKind::kSrdfGroup)
      .value("kSrdfGroupState",
             deployment::OccurrenceSymbolKind::kSrdfGroupState)
      .value("kSrdfEndEffector",
             deployment::OccurrenceSymbolKind::kSrdfEndEffector);

  py::enum_<deployment::OccurrenceReferenceKind>(
      module, "OccurrenceReferenceKind")
      .value("kJointParentLink",
             deployment::OccurrenceReferenceKind::kJointParentLink)
      .value("kJointChildLink",
             deployment::OccurrenceReferenceKind::kJointChildLink)
      .value("kJointMimic",
             deployment::OccurrenceReferenceKind::kJointMimic)
      .value("kVisualMaterial",
             deployment::OccurrenceReferenceKind::kVisualMaterial)
      .value("kSrdfGroupLink",
             deployment::OccurrenceReferenceKind::kSrdfGroupLink)
      .value("kSrdfGroupJoint",
             deployment::OccurrenceReferenceKind::kSrdfGroupJoint)
      .value("kSrdfChainBaseLink",
             deployment::OccurrenceReferenceKind::kSrdfChainBaseLink)
      .value("kSrdfChainTipLink",
             deployment::OccurrenceReferenceKind::kSrdfChainTipLink)
      .value("kSrdfSubgroup",
             deployment::OccurrenceReferenceKind::kSrdfSubgroup)
      .value("kSrdfGroupStateGroup",
             deployment::OccurrenceReferenceKind::kSrdfGroupStateGroup)
      .value("kSrdfGroupStateJoint",
             deployment::OccurrenceReferenceKind::kSrdfGroupStateJoint)
      .value("kSrdfEndEffectorParentLink",
             deployment::OccurrenceReferenceKind::
                 kSrdfEndEffectorParentLink)
      .value("kSrdfEndEffectorGroup",
             deployment::OccurrenceReferenceKind::kSrdfEndEffectorGroup)
      .value("kSrdfEndEffectorParentGroup",
             deployment::OccurrenceReferenceKind::
                 kSrdfEndEffectorParentGroup)
      .value("kSrdfPassiveJoint",
             deployment::OccurrenceReferenceKind::kSrdfPassiveJoint)
      .value("kSrdfDisabledCollisionFirstLink",
             deployment::OccurrenceReferenceKind::
                 kSrdfDisabledCollisionFirstLink)
      .value("kSrdfDisabledCollisionSecondLink",
             deployment::OccurrenceReferenceKind::
                 kSrdfDisabledCollisionSecondLink);

  py::enum_<deployment::CompositionCoordinateUnit>(
      module, "CompositionCoordinateUnit")
      .value("kNone", deployment::CompositionCoordinateUnit::kNone)
      .value("kRadians", deployment::CompositionCoordinateUnit::kRadians)
      .value("kMeters", deployment::CompositionCoordinateUnit::kMeters)
      .value("kPlanarMetersRadians",
             deployment::CompositionCoordinateUnit::kPlanarMetersRadians)
      .value("kFloatingMetersRadians",
             deployment::CompositionCoordinateUnit::kFloatingMetersRadians);

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
      .def_property_readonly(
          "value", [](const SourceVerifiedModelAsset& asset) {
            return asset.value();
          })
      .def_property_readonly(
          "qualified_asset",
          [](const SourceVerifiedModelAsset& asset) {
            return asset.qualified_asset();
          })
      .def_property_readonly(
          "retrieval_plan",
          [](const SourceVerifiedModelAsset& asset) {
            return asset.retrieval_plan();
          })
      .def_property_readonly(
          "verified_artifacts",
          [](const SourceVerifiedModelAsset& asset) {
            return immutable_snapshot(asset.verified_artifacts());
          })
      .def_property_readonly(
          "source_ir", [](const SourceVerifiedModelAsset& asset) {
            return asset.source_ir();
          });

  py::class_<deployment::OccurrenceSymbolKey>(
      module, "OccurrenceSymbolKey", py::is_final())
      .def_readonly("model_instance_id",
                    &deployment::OccurrenceSymbolKey::model_instance_id)
      .def_readonly("kind", &deployment::OccurrenceSymbolKey::kind)
      .def_readonly("scope_name",
                    &deployment::OccurrenceSymbolKey::scope_name)
      .def_readonly("source_name",
                    &deployment::OccurrenceSymbolKey::source_name)
      .def(py::self == py::self)
      .def(py::self != py::self);
  py::class_<deployment::OccurrenceSymbolRewrite>(
      module, "OccurrenceSymbolRewrite", py::is_final())
      .def_readonly("source", &deployment::OccurrenceSymbolRewrite::source)
      .def_readonly("compiled_name",
                    &deployment::OccurrenceSymbolRewrite::compiled_name)
      .def(py::self == py::self)
      .def(py::self != py::self);
  py::class_<deployment::OccurrenceReverseSymbolRewrite>(
      module, "OccurrenceReverseSymbolRewrite", py::is_final())
      .def_readonly(
          "compiled_name",
          &deployment::OccurrenceReverseSymbolRewrite::compiled_name)
      .def_readonly("source",
                    &deployment::OccurrenceReverseSymbolRewrite::source)
      .def(py::self == py::self)
      .def(py::self != py::self);
  py::class_<deployment::OccurrenceReferenceRewrite>(
      module, "OccurrenceReferenceRewrite", py::is_final())
      .def_readonly(
          "model_instance_id",
          &deployment::OccurrenceReferenceRewrite::model_instance_id)
      .def_readonly("kind", &deployment::OccurrenceReferenceRewrite::kind)
      .def_readonly(
          "owner_scope_name",
          &deployment::OccurrenceReferenceRewrite::owner_scope_name)
      .def_readonly(
          "owner_source_name",
          &deployment::OccurrenceReferenceRewrite::owner_source_name)
      .def_readonly(
          "reference_ordinal",
          &deployment::OccurrenceReferenceRewrite::reference_ordinal)
      .def_readonly(
          "source_target",
          &deployment::OccurrenceReferenceRewrite::source_target)
      .def_readonly(
          "compiled_target_name",
          &deployment::OccurrenceReferenceRewrite::compiled_target_name)
      .def(py::self == py::self)
      .def(py::self != py::self);
  py::class_<deployment::CompositionPlanningResourceRewrite>(
      module, "CompositionPlanningResourceRewrite", py::is_final())
      .def_readonly(
          "model_asset_id",
          &deployment::CompositionPlanningResourceRewrite::model_asset_id)
      .def_readonly(
          "model_revision",
          &deployment::CompositionPlanningResourceRewrite::model_revision)
      .def_readonly(
          "source_logical_uri",
          &deployment::CompositionPlanningResourceRewrite::source_logical_uri)
      .def_readonly(
          "compiled_logical_uri",
          &deployment::CompositionPlanningResourceRewrite::compiled_logical_uri)
      .def_readonly(
          "media_type",
          &deployment::CompositionPlanningResourceRewrite::media_type)
      .def_readonly(
          "byte_size",
          &deployment::CompositionPlanningResourceRewrite::byte_size)
      .def_readonly(
          "content_digest",
          &deployment::CompositionPlanningResourceRewrite::content_digest)
      .def_readonly(
          "content_addressed_store_key",
          &deployment::CompositionPlanningResourceRewrite::
              content_addressed_store_key)
      .def(py::self == py::self)
      .def(py::self != py::self);
  py::class_<deployment::CompositionModelOccurrence>(
      module, "CompositionModelOccurrence", py::is_final())
      .def_readonly(
          "model_instance_id",
          &deployment::CompositionModelOccurrence::model_instance_id)
      .def_readonly("model_asset_id",
                    &deployment::CompositionModelOccurrence::model_asset_id)
      .def_readonly("model_revision",
                    &deployment::CompositionModelOccurrence::model_revision)
      .def_readonly(
          "owner_component_id",
          &deployment::CompositionModelOccurrence::owner_component_id)
      .def_readonly(
          "owner_model_role_id",
          &deployment::CompositionModelOccurrence::owner_model_role_id)
      .def_readonly(
          "compiled_model_name",
          &deployment::CompositionModelOccurrence::compiled_model_name)
      .def_readonly(
          "compiled_robot_name",
          &deployment::CompositionModelOccurrence::compiled_robot_name)
      .def_readonly(
          "compiled_root_link_name",
          &deployment::CompositionModelOccurrence::compiled_root_link_name)
      .def(py::self == py::self)
      .def(py::self != py::self);
  py::class_<deployment::CompositionCellFrame>(
      module, "CompositionCellFrame", py::is_final())
      .def_readonly("cell_id", &deployment::CompositionCellFrame::cell_id)
      .def_readonly(
          "compiled_frame_name",
          &deployment::CompositionCellFrame::compiled_frame_name)
      .def(py::self == py::self)
      .def(py::self != py::self);
  py::class_<deployment::CompositionComponentModelFrame>(
      module, "CompositionComponentModelFrame", py::is_final())
      .def_readonly(
          "controllable_component_id",
          &deployment::CompositionComponentModelFrame::
              controllable_component_id)
      .def_readonly(
          "model_instance_id",
          &deployment::CompositionComponentModelFrame::model_instance_id)
      .def_readonly(
          "model_revision",
          &deployment::CompositionComponentModelFrame::model_revision)
      .def_readonly(
          "frame_role_id",
          &deployment::CompositionComponentModelFrame::frame_role_id)
      .def_readonly(
          "compiled_frame_name",
          &deployment::CompositionComponentModelFrame::compiled_frame_name)
      .def(py::self == py::self)
      .def(py::self != py::self);
  py::class_<deployment::CompositionPassiveModelFrame>(
      module, "CompositionPassiveModelFrame", py::is_final())
      .def_readonly(
          "model_instance_id",
          &deployment::CompositionPassiveModelFrame::model_instance_id)
      .def_readonly(
          "model_revision",
          &deployment::CompositionPassiveModelFrame::model_revision)
      .def_readonly(
          "frame_role_id",
          &deployment::CompositionPassiveModelFrame::frame_role_id)
      .def_readonly(
          "compiled_frame_name",
          &deployment::CompositionPassiveModelFrame::compiled_frame_name)
      .def(py::self == py::self)
      .def(py::self != py::self);

  py::tuple resolved_frame_types{3U};
  resolved_frame_types[0] = module.attr("CompositionCellFrame");
  resolved_frame_types[1] = module.attr("CompositionComponentModelFrame");
  resolved_frame_types[2] = module.attr("CompositionPassiveModelFrame");
  module.attr("CompositionResolvedFrame") =
      py::module_::import("typing")
          .attr("Union")
          .attr("__getitem__")(resolved_frame_types);

  py::class_<deployment::CompositionFrameResolution>(
      module, "CompositionFrameResolution", py::is_final())
      .def_readonly("frame", &deployment::CompositionFrameResolution::frame)
      .def_readonly(
          "source_link_name",
          &deployment::CompositionFrameResolution::source_link_name)
      .def(py::self == py::self)
      .def(py::self != py::self);
  py::class_<deployment::CompositionModelCoordinate>(
      module, "CompositionModelCoordinate", py::is_final())
      .def_readonly(
          "model_instance_id",
          &deployment::CompositionModelCoordinate::model_instance_id)
      .def_readonly(
          "owner_component_id",
          &deployment::CompositionModelCoordinate::owner_component_id)
      .def_readonly(
          "source_joint_index",
          &deployment::CompositionModelCoordinate::source_joint_index)
      .def_readonly(
          "source_joint_name",
          &deployment::CompositionModelCoordinate::source_joint_name)
      .def_readonly(
          "compiled_joint_name",
          &deployment::CompositionModelCoordinate::compiled_joint_name)
      .def_readonly("joint_type",
                    &deployment::CompositionModelCoordinate::joint_type)
      .def_readonly("coordinate_kind",
                    &deployment::CompositionModelCoordinate::coordinate_kind)
      .def_readonly("unit", &deployment::CompositionModelCoordinate::unit)
      .def_readonly(
          "mimic_source_compiled_joint_name",
          &deployment::CompositionModelCoordinate::
              mimic_source_compiled_joint_name)
      .def_readonly(
          "mimic_multiplier",
          &deployment::CompositionModelCoordinate::mimic_multiplier)
      .def_readonly("mimic_offset",
                    &deployment::CompositionModelCoordinate::mimic_offset)
      .def(py::self == py::self)
      .def(py::self != py::self);
  py::class_<deployment::CompositionCoordinateProjection>(
      module, "CompositionCoordinateProjection", py::is_final())
      .def_readonly(
          "model_instance_id",
          &deployment::CompositionCoordinateProjection::model_instance_id)
      .def_readonly(
          "source_joint_name",
          &deployment::CompositionCoordinateProjection::source_joint_name)
      .def_readonly(
          "compiled_joint_name",
          &deployment::CompositionCoordinateProjection::compiled_joint_name)
      .def_readonly(
          "coordinate_kind",
          &deployment::CompositionCoordinateProjection::coordinate_kind)
      .def_readonly("unit",
                    &deployment::CompositionCoordinateProjection::unit)
      .def_readonly(
          "multiplier",
          &deployment::CompositionCoordinateProjection::multiplier)
      .def_readonly("offset",
                    &deployment::CompositionCoordinateProjection::offset)
      .def_readonly(
          "derived_mimic",
          &deployment::CompositionCoordinateProjection::derived_mimic)
      .def(py::self == py::self)
      .def(py::self != py::self);
  py::class_<deployment::CompositionResourceCoordinateRow>(
      module, "CompositionResourceCoordinateRow", py::is_final())
      .def_readonly(
          "resource_role_id",
          &deployment::CompositionResourceCoordinateRow::resource_role_id)
      .def_readonly(
          "resource_id",
          &deployment::CompositionResourceCoordinateRow::resource_id)
      .def_property_readonly(
          "coordinates",
          [](const deployment::CompositionResourceCoordinateRow& value) {
            return immutable_snapshot(value.coordinates);
          })
      .def(py::self == py::self)
      .def(py::self != py::self);
  py::class_<deployment::CompositionControlBindingCoordinateMap>(
      module, "CompositionControlBindingCoordinateMap", py::is_final())
      .def_readonly(
          "control_binding_id",
          &deployment::CompositionControlBindingCoordinateMap::
              control_binding_id)
      .def_readonly(
          "controllable_component_id",
          &deployment::CompositionControlBindingCoordinateMap::
              controllable_component_id)
      .def_readonly(
          "command_target_id",
          &deployment::CompositionControlBindingCoordinateMap::
              command_target_id)
      .def_property_readonly(
          "resources",
          [](const deployment::CompositionControlBindingCoordinateMap& value) {
            return immutable_snapshot(value.resources);
          })
      .def(py::self == py::self)
      .def(py::self != py::self);
  py::class_<deployment::CompositionTargetGroupResolution>(
      module, "CompositionTargetGroupResolution", py::is_final())
      .def_readonly(
          "model_group_role_id",
          &deployment::CompositionTargetGroupResolution::model_group_role_id)
      .def_readonly(
          "model_instance_id",
          &deployment::CompositionTargetGroupResolution::model_instance_id)
      .def_readonly(
          "compiled_group_name",
          &deployment::CompositionTargetGroupResolution::compiled_group_name)
      .def_property_readonly(
          "expanded_compiled_link_names",
          [](const deployment::CompositionTargetGroupResolution& value) {
            return immutable_snapshot(value.expanded_compiled_link_names);
          })
      .def_property_readonly(
          "expanded_compiled_joint_names",
          [](const deployment::CompositionTargetGroupResolution& value) {
            return immutable_snapshot(value.expanded_compiled_joint_names);
          })
      .def(py::self == py::self)
      .def(py::self != py::self);
  py::class_<deployment::CompositionTargetFrameResolution>(
      module, "CompositionTargetFrameResolution", py::is_final())
      .def_readonly(
          "frame_role_id",
          &deployment::CompositionTargetFrameResolution::frame_role_id)
      .def_readonly("frame",
                    &deployment::CompositionTargetFrameResolution::frame)
      .def(py::self == py::self)
      .def(py::self != py::self);
  py::class_<deployment::CompositionTargetCollisionResolution>(
      module, "CompositionTargetCollisionResolution", py::is_final())
      .def_readonly(
          "collision_body_role_id",
          &deployment::CompositionTargetCollisionResolution::
              collision_body_role_id)
      .def_readonly(
          "model_instance_id",
          &deployment::CompositionTargetCollisionResolution::model_instance_id)
      .def_property_readonly(
          "compiled_link_names",
          [](const deployment::CompositionTargetCollisionResolution& value) {
            return immutable_snapshot(value.compiled_link_names);
          })
      .def(py::self == py::self)
      .def(py::self != py::self);
  py::class_<deployment::CompositionTargetResolution>(
      module, "CompositionTargetResolution", py::is_final())
      .def_readonly(
          "controllable_component_id",
          &deployment::CompositionTargetResolution::controllable_component_id)
      .def_readonly(
          "command_target_id",
          &deployment::CompositionTargetResolution::command_target_id)
      .def_property_readonly(
          "model_groups",
          [](const deployment::CompositionTargetResolution& value) {
            return immutable_snapshot(value.model_groups);
          })
      .def_property_readonly(
          "frames",
          [](const deployment::CompositionTargetResolution& value) {
            return immutable_snapshot(value.frames);
          })
      .def_property_readonly(
          "collision_bodies",
          [](const deployment::CompositionTargetResolution& value) {
            return immutable_snapshot(value.collision_bodies);
          })
      .def(py::self == py::self)
      .def(py::self != py::self);
  py::class_<deployment::CompositionDirectModelAnchor>(
      module, "CompositionDirectModelAnchor", py::is_final())
      .def_readonly(
          "model_instance_id",
          &deployment::CompositionDirectModelAnchor::model_instance_id)
      .def_readonly(
          "root_pose_in_cell",
          &deployment::CompositionDirectModelAnchor::root_pose_in_cell)
      .def(py::self == py::self)
      .def(py::self != py::self);
  py::class_<deployment::CompositionDirectComponentAnchor>(
      module, "CompositionDirectComponentAnchor", py::is_final())
      .def_readonly(
          "controllable_component_id",
          &deployment::CompositionDirectComponentAnchor::
              controllable_component_id)
      .def_readonly(
          "root_model_instance_id",
          &deployment::CompositionDirectComponentAnchor::root_model_instance_id)
      .def_readonly(
          "root_pose_in_cell",
          &deployment::CompositionDirectComponentAnchor::root_pose_in_cell)
      .def(py::self == py::self)
      .def(py::self != py::self);
  py::class_<deployment::CompositionModelMount>(
      module, "CompositionModelMount", py::is_final())
      .def_readonly(
          "controllable_component_id",
          &deployment::CompositionModelMount::controllable_component_id)
      .def_readonly(
          "parent_model_instance_id",
          &deployment::CompositionModelMount::parent_model_instance_id)
      .def_readonly(
          "child_model_instance_id",
          &deployment::CompositionModelMount::child_model_instance_id)
      .def_readonly(
          "child_frame_pose_in_parent_frame",
          &deployment::CompositionModelMount::
              child_frame_pose_in_parent_frame)
      .def(py::self == py::self)
      .def(py::self != py::self);
  py::class_<deployment::CompositionComponentAttachment>(
      module, "CompositionComponentAttachment", py::is_final())
      .def_readonly(
          "component_attachment_id",
          &deployment::CompositionComponentAttachment::component_attachment_id)
      .def_readonly(
          "parent_component_id",
          &deployment::CompositionComponentAttachment::parent_component_id)
      .def_readonly(
          "child_component_id",
          &deployment::CompositionComponentAttachment::child_component_id)
      .def_readonly(
          "child_interface_pose_in_parent_interface",
          &deployment::CompositionComponentAttachment::
              child_interface_pose_in_parent_interface)
      .def(py::self == py::self)
      .def(py::self != py::self);

  py::tuple connection_source_types{4U};
  connection_source_types[0] = module.attr("CompositionDirectModelAnchor");
  connection_source_types[1] =
      module.attr("CompositionDirectComponentAnchor");
  connection_source_types[2] = module.attr("CompositionModelMount");
  connection_source_types[3] =
      module.attr("CompositionComponentAttachment");
  module.attr("CompositionConnectionSource") =
      py::module_::import("typing")
          .attr("Union")
          .attr("__getitem__")(connection_source_types);

  py::class_<deployment::CompositionFixedConnection>(
      module, "CompositionFixedConnection", py::is_final())
      .def_readonly("source",
                    &deployment::CompositionFixedConnection::source)
      .def_readonly(
          "compiled_joint_name",
          &deployment::CompositionFixedConnection::compiled_joint_name)
      .def_readonly(
          "compiled_parent_link_name",
          &deployment::CompositionFixedConnection::compiled_parent_link_name)
      .def_readonly(
          "compiled_child_link_name",
          &deployment::CompositionFixedConnection::compiled_child_link_name)
      .def_readonly(
          "child_pose_in_parent",
          &deployment::CompositionFixedConnection::child_pose_in_parent)
      .def(py::self == py::self)
      .def(py::self != py::self);
  py::class_<deployment::CompositionFixedPathSegment>(
      module, "CompositionFixedPathSegment", py::is_final())
      .def_readonly(
          "fixed_connection_index",
          &deployment::CompositionFixedPathSegment::fixed_connection_index)
      .def(py::self == py::self)
      .def(py::self != py::self);
  py::class_<deployment::CompositionModelJointPathSegment>(
      module, "CompositionModelJointPathSegment", py::is_final())
      .def_readonly(
          "model_instance_id",
          &deployment::CompositionModelJointPathSegment::model_instance_id)
      .def_readonly(
          "composition_coordinate_index",
          &deployment::CompositionModelJointPathSegment::
              composition_coordinate_index)
      .def_readonly(
          "source_joint_name",
          &deployment::CompositionModelJointPathSegment::source_joint_name)
      .def_readonly(
          "compiled_joint_name",
          &deployment::CompositionModelJointPathSegment::compiled_joint_name)
      .def_readonly(
          "compiled_parent_link_name",
          &deployment::CompositionModelJointPathSegment::
              compiled_parent_link_name)
      .def_readonly(
          "compiled_child_link_name",
          &deployment::CompositionModelJointPathSegment::
              compiled_child_link_name)
      .def(py::self == py::self)
      .def(py::self != py::self);

  py::tuple frame_path_segment_types{2U};
  frame_path_segment_types[0] = module.attr("CompositionFixedPathSegment");
  frame_path_segment_types[1] =
      module.attr("CompositionModelJointPathSegment");
  module.attr("CompositionFramePathSegment") =
      py::module_::import("typing")
          .attr("Union")
          .attr("__getitem__")(frame_path_segment_types);

  py::class_<deployment::CompositionFramePath>(
      module, "CompositionFramePath", py::is_final())
      .def_readonly(
          "terminal_frame",
          &deployment::CompositionFramePath::terminal_frame)
      .def_property_readonly(
          "segments", [](const deployment::CompositionFramePath& value) {
            return immutable_snapshot(value.segments);
          })
      .def(py::self == py::self)
      .def(py::self != py::self);

  py::class_<deployment::CompositionQualifiedCellIr>(
      module, "CompositionQualifiedCellIr", py::is_final())
      .def_property_readonly(
          "cell_frame",
          [](const deployment::CompositionQualifiedCellIr& value) {
            return value.cell_frame();
          })
      .def_property_readonly(
          "component_types",
          [](const deployment::CompositionQualifiedCellIr& value) {
            return immutable_snapshot(value.component_types());
          })
      .def_property_readonly(
          "control_binding_coordinate_maps",
          [](const deployment::CompositionQualifiedCellIr& value) {
            return immutable_snapshot(value.control_binding_coordinate_maps());
          })
      .def_property_readonly(
          "coordinates",
          [](const deployment::CompositionQualifiedCellIr& value) {
            return immutable_snapshot(value.coordinates());
          })
      .def_property_readonly(
          "fixed_connections",
          [](const deployment::CompositionQualifiedCellIr& value) {
            return immutable_snapshot(value.fixed_connections());
          })
      .def_property_readonly(
          "flattened_srdf",
          [](const deployment::CompositionQualifiedCellIr& value) {
            return value.flattened_srdf();
          })
      .def_property_readonly(
          "flattened_urdf",
          [](const deployment::CompositionQualifiedCellIr& value) {
            return value.flattened_urdf();
          })
      .def_property_readonly(
          "frame_paths",
          [](const deployment::CompositionQualifiedCellIr& value) {
            return immutable_snapshot(value.frame_paths());
          })
      .def_property_readonly(
          "frames",
          [](const deployment::CompositionQualifiedCellIr& value) {
            return immutable_snapshot(value.frames());
          })
      .def_property_readonly(
          "graph", [](const deployment::CompositionQualifiedCellIr& value) {
            return value.graph();
          })
      .def_property_readonly(
          "model_assets",
          [](const deployment::CompositionQualifiedCellIr& value) {
            return immutable_snapshot(value.model_assets());
          })
      .def_property_readonly(
          "model_occurrences",
          [](const deployment::CompositionQualifiedCellIr& value) {
            return immutable_snapshot(value.model_occurrences());
          })
      .def_property_readonly(
          "planning_resource_rewrites",
          [](const deployment::CompositionQualifiedCellIr& value) {
            return immutable_snapshot(value.planning_resource_rewrites());
          })
      .def_property_readonly(
          "reference_rewrites",
          [](const deployment::CompositionQualifiedCellIr& value) {
            return immutable_snapshot(value.reference_rewrites());
          })
      .def_property_readonly(
          "reverse_symbol_rewrites",
          [](const deployment::CompositionQualifiedCellIr& value) {
            return immutable_snapshot(value.reverse_symbol_rewrites());
          })
      .def_property_readonly(
          "symbol_rewrites",
          [](const deployment::CompositionQualifiedCellIr& value) {
            return immutable_snapshot(value.symbol_rewrites());
          })
      .def_property_readonly(
          "targets",
          [](const deployment::CompositionQualifiedCellIr& value) {
            return immutable_snapshot(value.targets());
          })
      .def_property_readonly(
          "verified_graph",
          [](const deployment::CompositionQualifiedCellIr& value) {
            return value.verified_graph();
          });

  py::class_<deployment::ExpandedControlInterfaceClaim>(
      module, "ExpandedControlInterfaceClaim", py::is_final())
      .def_readonly(
          "resource_role_id",
          &deployment::ExpandedControlInterfaceClaim::resource_role_id)
      .def_readonly("resource_id",
                    &deployment::ExpandedControlInterfaceClaim::resource_id)
      .def_readonly(
          "interface_name",
          &deployment::ExpandedControlInterfaceClaim::interface_name)
      .def_readonly(
          "logical_interface_symbol",
          &deployment::ExpandedControlInterfaceClaim::
              logical_interface_symbol)
      .def(py::self == py::self)
      .def(py::self != py::self);
  py::class_<deployment::LogicalBindingInterfaceTemplate>(
      module, "LogicalBindingInterfaceTemplate", py::is_final())
      .def_readonly(
          "control_binding_id",
          &deployment::LogicalBindingInterfaceTemplate::control_binding_id)
      .def_readonly(
          "control_binding_revision",
          &deployment::LogicalBindingInterfaceTemplate::
              control_binding_revision)
      .def_readonly(
          "control_interface_contract_id",
          &deployment::LogicalBindingInterfaceTemplate::
              control_interface_contract_id)
      .def_readonly(
          "control_interface_revision",
          &deployment::LogicalBindingInterfaceTemplate::
              control_interface_revision)
      .def_property_readonly(
          "command_claims",
          [](const deployment::LogicalBindingInterfaceTemplate& value) {
            return immutable_snapshot(value.command_claims);
          })
      .def_property_readonly(
          "state_claims",
          [](const deployment::LogicalBindingInterfaceTemplate& value) {
            return immutable_snapshot(value.state_claims);
          })
      .def(py::self == py::self)
      .def(py::self != py::self);
  py::class_<deployment::LogicalControllerBindingProjection>(
      module, "LogicalControllerBindingProjection", py::is_final())
      .def_readonly(
          "control_binding_id",
          &deployment::LogicalControllerBindingProjection::control_binding_id)
      .def_readonly(
          "control_binding_revision",
          &deployment::LogicalControllerBindingProjection::
              control_binding_revision)
      .def_readonly(
          "control_route_id",
          &deployment::LogicalControllerBindingProjection::control_route_id)
      .def_readonly(
          "activation_set_id",
          &deployment::LogicalControllerBindingProjection::activation_set_id)
      .def_property_readonly(
          "claimed_directions",
          [](const deployment::LogicalControllerBindingProjection& value) {
            return immutable_snapshot(value.claimed_directions);
          })
      .def_property_readonly(
          "exported_reference_interfaces",
          [](const deployment::LogicalControllerBindingProjection& value) {
            return immutable_snapshot(value.exported_reference_interfaces);
          })
      .def_property_readonly(
          "exported_state_interfaces",
          [](const deployment::LogicalControllerBindingProjection& value) {
            return immutable_snapshot(value.exported_state_interfaces);
          })
      .def(py::self == py::self)
      .def(py::self != py::self);
  py::class_<deployment::LogicalControllerTemplate>(
      module, "LogicalControllerTemplate", py::is_final())
      .def_readonly(
          "control_domain_id",
          &deployment::LogicalControllerTemplate::control_domain_id)
      .def_readonly(
          "control_domain_revision",
          &deployment::LogicalControllerTemplate::control_domain_revision)
      .def_readonly(
          "controller_topology_id",
          &deployment::LogicalControllerTemplate::controller_topology_id)
      .def_readonly(
          "topology_revision",
          &deployment::LogicalControllerTemplate::topology_revision)
      .def_readonly(
          "controller_role_id",
          &deployment::LogicalControllerTemplate::controller_role_id)
      .def_readonly(
          "logical_controller_symbol",
          &deployment::LogicalControllerTemplate::logical_controller_symbol)
      .def_property_readonly(
          "binding_projections",
          [](const deployment::LogicalControllerTemplate& value) {
            return immutable_snapshot(value.binding_projections);
          })
      .def(py::self == py::self)
      .def(py::self != py::self);
  py::class_<deployment::LogicalRouteEndpointTemplate>(
      module, "LogicalRouteEndpointTemplate", py::is_final())
      .def_readonly(
          "endpoint_role_id",
          &deployment::LogicalRouteEndpointTemplate::endpoint_role_id)
      .def_readonly(
          "logical_endpoint_symbol",
          &deployment::LogicalRouteEndpointTemplate::logical_endpoint_symbol)
      .def_readonly(
          "is_command_endpoint",
          &deployment::LogicalRouteEndpointTemplate::is_command_endpoint)
      .def_readonly(
          "is_state_endpoint",
          &deployment::LogicalRouteEndpointTemplate::is_state_endpoint)
      .def(py::self == py::self)
      .def(py::self != py::self);
  py::class_<deployment::LogicalControlRouteTemplate>(
      module, "LogicalControlRouteTemplate", py::is_final())
      .def_readonly(
          "control_binding_id",
          &deployment::LogicalControlRouteTemplate::control_binding_id)
      .def_readonly(
          "control_binding_revision",
          &deployment::LogicalControlRouteTemplate::control_binding_revision)
      .def_readonly(
          "control_domain_id",
          &deployment::LogicalControlRouteTemplate::control_domain_id)
      .def_readonly(
          "control_domain_revision",
          &deployment::LogicalControlRouteTemplate::control_domain_revision)
      .def_readonly(
          "controller_topology_id",
          &deployment::LogicalControlRouteTemplate::controller_topology_id)
      .def_readonly(
          "topology_revision",
          &deployment::LogicalControlRouteTemplate::topology_revision)
      .def_readonly(
          "control_route_id",
          &deployment::LogicalControlRouteTemplate::control_route_id)
      .def_readonly(
          "logical_route_symbol",
          &deployment::LogicalControlRouteTemplate::logical_route_symbol)
      .def_property_readonly(
          "controller_role_ids",
          [](const deployment::LogicalControlRouteTemplate& value) {
            return immutable_snapshot(value.controller_role_ids);
          })
      .def_property_readonly(
          "logical_controller_symbols",
          [](const deployment::LogicalControlRouteTemplate& value) {
            return immutable_snapshot(value.logical_controller_symbols);
          })
      .def_property_readonly(
          "path_edges",
          [](const deployment::LogicalControlRouteTemplate& value) {
            return immutable_snapshot(value.path_edges);
          })
      .def_property_readonly(
          "compatible_activation_set_ids",
          [](const deployment::LogicalControlRouteTemplate& value) {
            return immutable_snapshot(value.compatible_activation_set_ids);
          })
      .def_property_readonly(
          "endpoint_templates",
          [](const deployment::LogicalControlRouteTemplate& value) {
            return immutable_snapshot(value.endpoint_templates);
          })
      .def(py::self == py::self)
      .def(py::self != py::self);
  py::class_<deployment::ControlBindingCapabilityOwner>(
      module, "ControlBindingCapabilityOwner", py::is_final())
      .def_readonly(
          "control_binding_id",
          &deployment::ControlBindingCapabilityOwner::control_binding_id)
      .def_readonly(
          "control_binding_revision",
          &deployment::ControlBindingCapabilityOwner::
              control_binding_revision)
      .def(py::self == py::self)
      .def(py::self != py::self);
  py::class_<deployment::CapabilityProviderCapabilityOwner>(
      module, "CapabilityProviderCapabilityOwner", py::is_final())
      .def_readonly(
          "capability_provider_binding_id",
          &deployment::CapabilityProviderCapabilityOwner::
              capability_provider_binding_id)
      .def_readonly(
          "capability_provider_binding_revision",
          &deployment::CapabilityProviderCapabilityOwner::
              capability_provider_binding_revision)
      .def(py::self == py::self)
      .def(py::self != py::self);

  py::tuple capability_owner_types{2U};
  capability_owner_types[0] = module.attr("ControlBindingCapabilityOwner");
  capability_owner_types[1] =
      module.attr("CapabilityProviderCapabilityOwner");
  module.attr("QualifiedCapabilityOwner") =
      py::module_::import("typing")
          .attr("Union")
          .attr("__getitem__")(capability_owner_types);

  py::class_<deployment::LogicalCapabilityTemplate>(
      module, "LogicalCapabilityTemplate", py::is_final())
      .def_readonly("owner", &deployment::LogicalCapabilityTemplate::owner)
      .def_readonly(
          "capability_id",
          &deployment::LogicalCapabilityTemplate::capability_id)
      .def_readonly(
          "capability_definition_revision",
          &deployment::LogicalCapabilityTemplate::
              capability_definition_revision)
      .def_readonly("scope", &deployment::LogicalCapabilityTemplate::scope)
      .def_readonly(
          "provider_binding_kind",
          &deployment::LogicalCapabilityTemplate::provider_binding_kind)
      .def_readonly(
          "provider_instance_id",
          &deployment::LogicalCapabilityTemplate::provider_instance_id)
      .def_readonly(
          "provider_manifest_id",
          &deployment::LogicalCapabilityTemplate::provider_manifest_id)
      .def_readonly(
          "provider_manifest_revision",
          &deployment::LogicalCapabilityTemplate::
              provider_manifest_revision)
      .def_readonly(
          "logical_capability_symbol",
          &deployment::LogicalCapabilityTemplate::logical_capability_symbol)
      .def(py::self == py::self)
      .def(py::self != py::self);
  py::class_<deployment::RuntimeEndpointSourceTemplate>(
      module, "RuntimeEndpointSourceTemplate", py::is_final())
      .def_readonly(
          "runtime_unit_id",
          &deployment::RuntimeEndpointSourceTemplate::runtime_unit_id)
      .def_readonly(
          "runtime_profile_id",
          &deployment::RuntimeEndpointSourceTemplate::runtime_profile_id)
      .def_readonly(
          "runtime_profile_revision",
          &deployment::RuntimeEndpointSourceTemplate::
              runtime_profile_revision)
      .def_readonly(
          "source_endpoint_role_id",
          &deployment::RuntimeEndpointSourceTemplate::source_endpoint_role_id)
      .def(py::self == py::self)
      .def(py::self != py::self);
  py::class_<deployment::ExternalEndpointSourceTemplate>(
      module, "ExternalEndpointSourceTemplate", py::is_final())
      .def_readonly(
          "external_resource_alias",
          &deployment::ExternalEndpointSourceTemplate::external_resource_alias)
      .def_readonly(
          "resource_kind",
          &deployment::ExternalEndpointSourceTemplate::resource_kind)
      .def_readonly(
          "provider_instance_id",
          &deployment::ExternalEndpointSourceTemplate::provider_instance_id)
      .def_readonly(
          "provider_manifest_id",
          &deployment::ExternalEndpointSourceTemplate::provider_manifest_id)
      .def_readonly(
          "provider_manifest_revision",
          &deployment::ExternalEndpointSourceTemplate::
              provider_manifest_revision)
      .def_readonly(
          "endpoint_role_id",
          &deployment::ExternalEndpointSourceTemplate::endpoint_role_id)
      .def_readonly(
          "site_overlay_value_ref",
          &deployment::ExternalEndpointSourceTemplate::site_overlay_value_ref)
      .def_readonly(
          "selected_site_overlay_id",
          &deployment::ExternalEndpointSourceTemplate::
              selected_site_overlay_id)
      .def_readonly(
          "selected_site_overlay_revision",
          &deployment::ExternalEndpointSourceTemplate::
              selected_site_overlay_revision)
      .def_readonly(
          "selected_site_overlay_non_secret_digest",
          &deployment::ExternalEndpointSourceTemplate::
              selected_site_overlay_non_secret_digest)
      .def(py::self == py::self)
      .def(py::self != py::self);

  py::tuple endpoint_source_types{2U};
  endpoint_source_types[0] = module.attr("RuntimeEndpointSourceTemplate");
  endpoint_source_types[1] = module.attr("ExternalEndpointSourceTemplate");
  module.attr("LogicalEndpointSourceTemplate") =
      py::module_::import("typing")
          .attr("Union")
          .attr("__getitem__")(endpoint_source_types);

  py::class_<deployment::LogicalProviderEndpointTemplate>(
      module, "LogicalProviderEndpointTemplate", py::is_final())
      .def_readonly(
          "capability_provider_binding_id",
          &deployment::LogicalProviderEndpointTemplate::
              capability_provider_binding_id)
      .def_readonly(
          "capability_provider_binding_revision",
          &deployment::LogicalProviderEndpointTemplate::
              capability_provider_binding_revision)
      .def_readonly(
          "endpoint_role_id",
          &deployment::LogicalProviderEndpointTemplate::endpoint_role_id)
      .def_readonly(
          "provider_instance_id",
          &deployment::LogicalProviderEndpointTemplate::provider_instance_id)
      .def_readonly(
          "provider_manifest_id",
          &deployment::LogicalProviderEndpointTemplate::provider_manifest_id)
      .def_readonly(
          "provider_manifest_revision",
          &deployment::LogicalProviderEndpointTemplate::
              provider_manifest_revision)
      .def_readonly("source",
                    &deployment::LogicalProviderEndpointTemplate::source)
      .def_readonly(
          "logical_endpoint_symbol",
          &deployment::LogicalProviderEndpointTemplate::
              logical_endpoint_symbol)
      .def(py::self == py::self)
      .def(py::self != py::self);
  py::class_<deployment::LogicalExternalResourceTemplate>(
      module, "LogicalExternalResourceTemplate", py::is_final())
      .def_readonly(
          "external_resource_alias",
          &deployment::LogicalExternalResourceTemplate::
              external_resource_alias)
      .def_readonly(
          "resource_kind",
          &deployment::LogicalExternalResourceTemplate::resource_kind)
      .def_readonly(
          "provider_instance_id",
          &deployment::LogicalExternalResourceTemplate::provider_instance_id)
      .def_readonly(
          "provider_manifest_id",
          &deployment::LogicalExternalResourceTemplate::provider_manifest_id)
      .def_readonly(
          "provider_manifest_revision",
          &deployment::LogicalExternalResourceTemplate::
              provider_manifest_revision)
      .def_readonly(
          "endpoint_role_id",
          &deployment::LogicalExternalResourceTemplate::endpoint_role_id)
      .def_readonly(
          "site_overlay_value_ref",
          &deployment::LogicalExternalResourceTemplate::
              site_overlay_value_ref)
      .def_readonly(
          "selected_site_overlay_id",
          &deployment::LogicalExternalResourceTemplate::
              selected_site_overlay_id)
      .def_readonly(
          "selected_site_overlay_revision",
          &deployment::LogicalExternalResourceTemplate::
              selected_site_overlay_revision)
      .def_readonly(
          "selected_site_overlay_non_secret_digest",
          &deployment::LogicalExternalResourceTemplate::
              selected_site_overlay_non_secret_digest)
      .def_readonly(
          "logical_external_resource_symbol",
          &deployment::LogicalExternalResourceTemplate::
              logical_external_resource_symbol)
      .def(py::self == py::self)
      .def(py::self != py::self);
  py::class_<deployment::LogicalStopProviderCommandEndpoint>(
      module, "LogicalStopProviderCommandEndpoint", py::is_final())
      .def_readonly(
          "stop_escalation_stage_id",
          &deployment::LogicalStopProviderCommandEndpoint::
              stop_escalation_stage_id)
      .def_readonly(
          "provider_contract_ref",
          &deployment::LogicalStopProviderCommandEndpoint::
              provider_contract_ref)
      .def_readonly(
          "endpoint_role_id",
          &deployment::LogicalStopProviderCommandEndpoint::endpoint_role_id)
      .def_readonly(
          "logical_endpoint_symbol",
          &deployment::LogicalStopProviderCommandEndpoint::
              logical_endpoint_symbol)
      .def(py::self == py::self)
      .def(py::self != py::self);
  py::class_<deployment::LogicalStopProviderEvidenceEndpoint>(
      module, "LogicalStopProviderEvidenceEndpoint", py::is_final())
      .def_readonly(
          "stop_escalation_stage_id",
          &deployment::LogicalStopProviderEvidenceEndpoint::
              stop_escalation_stage_id)
      .def_readonly(
          "evidence_item_id",
          &deployment::LogicalStopProviderEvidenceEndpoint::evidence_item_id)
      .def_readonly(
          "provider_contract_ref",
          &deployment::LogicalStopProviderEvidenceEndpoint::
              provider_contract_ref)
      .def_readonly(
          "endpoint_role_id",
          &deployment::LogicalStopProviderEvidenceEndpoint::endpoint_role_id)
      .def_readonly(
          "logical_endpoint_symbol",
          &deployment::LogicalStopProviderEvidenceEndpoint::
              logical_endpoint_symbol)
      .def(py::self == py::self)
      .def(py::self != py::self);
  py::class_<deployment::LogicalStopBindingStateAuthoritySource>(
      module, "LogicalStopBindingStateAuthoritySource", py::is_final())
      .def_readonly(
          "stop_escalation_stage_id",
          &deployment::LogicalStopBindingStateAuthoritySource::
              stop_escalation_stage_id)
      .def_readonly(
          "evidence_item_id",
          &deployment::LogicalStopBindingStateAuthoritySource::evidence_item_id)
      .def_readonly(
          "state_authority_id",
          &deployment::LogicalStopBindingStateAuthoritySource::
              state_authority_id)
      .def_readonly(
          "state_authority_class",
          &deployment::LogicalStopBindingStateAuthoritySource::
              state_authority_class)
      .def_readonly(
          "state_endpoint_role_id",
          &deployment::LogicalStopBindingStateAuthoritySource::
              state_endpoint_role_id)
      .def_readonly(
          "logical_endpoint_symbol",
          &deployment::LogicalStopBindingStateAuthoritySource::
              logical_endpoint_symbol)
      .def(py::self == py::self)
      .def(py::self != py::self);
  py::class_<deployment::LogicalResourceStopScopeTemplate>(
      module, "LogicalResourceStopScopeTemplate", py::is_final())
      .def_readonly(
          "stop_escalation_stage_id",
          &deployment::LogicalResourceStopScopeTemplate::
              stop_escalation_stage_id)
      .def_readonly(
          "activation_set_id",
          &deployment::LogicalResourceStopScopeTemplate::activation_set_id)
      .def_property_readonly(
          "resource_ids",
          [](const deployment::LogicalResourceStopScopeTemplate& value) {
            return immutable_snapshot(value.resource_ids);
          })
      .def(py::self == py::self)
      .def(py::self != py::self);
  py::class_<deployment::LogicalControllerSetStopScopeTemplate>(
      module, "LogicalControllerSetStopScopeTemplate", py::is_final())
      .def_readonly(
          "stop_escalation_stage_id",
          &deployment::LogicalControllerSetStopScopeTemplate::
              stop_escalation_stage_id)
      .def_readonly(
          "activation_set_id",
          &deployment::LogicalControllerSetStopScopeTemplate::
              activation_set_id)
      .def_property_readonly(
          "active_controller_role_ids",
          [](const deployment::LogicalControllerSetStopScopeTemplate& value) {
            return immutable_snapshot(value.active_controller_role_ids);
          })
      .def_property_readonly(
          "logical_controller_symbols",
          [](const deployment::LogicalControllerSetStopScopeTemplate& value) {
            return immutable_snapshot(value.logical_controller_symbols);
          })
      .def(py::self == py::self)
      .def(py::self != py::self);
  py::class_<deployment::LogicalControlDomainStopScopeTemplate>(
      module, "LogicalControlDomainStopScopeTemplate", py::is_final())
      .def_readonly(
          "stop_escalation_stage_id",
          &deployment::LogicalControlDomainStopScopeTemplate::
              stop_escalation_stage_id)
      .def_readonly(
          "activation_set_id",
          &deployment::LogicalControlDomainStopScopeTemplate::
              activation_set_id)
      .def_readonly(
          "control_domain_id",
          &deployment::LogicalControlDomainStopScopeTemplate::
              control_domain_id)
      .def_readonly(
          "control_domain_revision",
          &deployment::LogicalControlDomainStopScopeTemplate::
              control_domain_revision)
      .def(py::self == py::self)
      .def(py::self != py::self);

  py::tuple stop_scope_types{3U};
  stop_scope_types[0] = module.attr("LogicalResourceStopScopeTemplate");
  stop_scope_types[1] =
      module.attr("LogicalControllerSetStopScopeTemplate");
  stop_scope_types[2] =
      module.attr("LogicalControlDomainStopScopeTemplate");
  module.attr("LogicalStopScopeTemplate") =
      py::module_::import("typing")
          .attr("Union")
          .attr("__getitem__")(stop_scope_types);

  py::class_<deployment::LogicalStopEscalationStageTemplate>(
      module, "LogicalStopEscalationStageTemplate", py::is_final())
      .def_readonly(
          "stop_escalation_stage_id",
          &deployment::LogicalStopEscalationStageTemplate::
              stop_escalation_stage_id)
      .def_readonly(
          "stage_kind",
          &deployment::LogicalStopEscalationStageTemplate::stage_kind)
      .def_readonly(
          "containment_granularity",
          &deployment::LogicalStopEscalationStageTemplate::
              containment_granularity)
      .def_readonly(
          "containment_command",
          &deployment::LogicalStopEscalationStageTemplate::
              containment_command)
      .def_readonly(
          "evidence_conjunction",
          &deployment::LogicalStopEscalationStageTemplate::
              evidence_conjunction)
      .def_property_readonly(
          "all_of_evidence_requirements",
          [](const deployment::LogicalStopEscalationStageTemplate& value) {
            return immutable_snapshot(value.all_of_evidence_requirements);
          })
      .def_readonly(
          "default_contract_parameters",
          &deployment::LogicalStopEscalationStageTemplate::
              default_contract_parameters)
      .def_readonly(
          "commissioned_stop_interface",
          &deployment::LogicalStopEscalationStageTemplate::
              commissioned_stop_interface)
      .def_readonly(
          "provider_command_endpoint",
          &deployment::LogicalStopEscalationStageTemplate::
              provider_command_endpoint)
      .def_property_readonly(
          "provider_evidence_endpoints",
          [](const deployment::LogicalStopEscalationStageTemplate& value) {
            return immutable_snapshot(value.provider_evidence_endpoints);
          })
      .def_property_readonly(
          "binding_state_authority_sources",
          [](const deployment::LogicalStopEscalationStageTemplate& value) {
            return immutable_snapshot(value.binding_state_authority_sources);
          })
      .def_property_readonly(
          "containment_scopes",
          [](const deployment::LogicalStopEscalationStageTemplate& value) {
            return immutable_snapshot(value.containment_scopes);
          })
      .def(py::self == py::self)
      .def(py::self != py::self);

  py::class_<deployment::LogicalNotStoppableApplicabilityTemplate>(
      module, "LogicalNotStoppableApplicabilityTemplate", py::is_final())
      .def_readonly(
          "control_binding_id",
          &deployment::LogicalNotStoppableApplicabilityTemplate::
              control_binding_id)
      .def_readonly(
          "control_binding_revision",
          &deployment::LogicalNotStoppableApplicabilityTemplate::
              control_binding_revision)
      .def_readonly(
          "control_route_id",
          &deployment::LogicalNotStoppableApplicabilityTemplate::
              control_route_id)
      .def_readonly(
          "stop_evidence_policy_id",
          &deployment::LogicalNotStoppableApplicabilityTemplate::
              stop_evidence_policy_id)
      .def_readonly(
          "stop_evidence_policy_revision",
          &deployment::LogicalNotStoppableApplicabilityTemplate::
              stop_evidence_policy_revision)
      .def_readonly(
          "control_domain_id",
          &deployment::LogicalNotStoppableApplicabilityTemplate::
              control_domain_id)
      .def_readonly(
          "control_domain_revision",
          &deployment::LogicalNotStoppableApplicabilityTemplate::
              control_domain_revision)
      .def_readonly(
          "controller_topology_id",
          &deployment::LogicalNotStoppableApplicabilityTemplate::
              controller_topology_id)
      .def_readonly(
          "topology_revision",
          &deployment::LogicalNotStoppableApplicabilityTemplate::
              topology_revision)
      .def_readonly(
          "containment_granularity",
          &deployment::LogicalNotStoppableApplicabilityTemplate::
              containment_granularity)
      .def_readonly(
          "disposition",
          &deployment::LogicalNotStoppableApplicabilityTemplate::disposition)
      .def(py::self == py::self)
      .def(py::self != py::self);
  py::class_<deployment::LogicalStoppableApplicabilityTemplate>(
      module, "LogicalStoppableApplicabilityTemplate", py::is_final())
      .def_readonly(
          "control_binding_id",
          &deployment::LogicalStoppableApplicabilityTemplate::
              control_binding_id)
      .def_readonly(
          "control_binding_revision",
          &deployment::LogicalStoppableApplicabilityTemplate::
              control_binding_revision)
      .def_readonly(
          "control_route_id",
          &deployment::LogicalStoppableApplicabilityTemplate::control_route_id)
      .def_readonly(
          "stop_evidence_policy_id",
          &deployment::LogicalStoppableApplicabilityTemplate::
              stop_evidence_policy_id)
      .def_readonly(
          "stop_evidence_policy_revision",
          &deployment::LogicalStoppableApplicabilityTemplate::
              stop_evidence_policy_revision)
      .def_readonly(
          "control_domain_id",
          &deployment::LogicalStoppableApplicabilityTemplate::control_domain_id)
      .def_readonly(
          "control_domain_revision",
          &deployment::LogicalStoppableApplicabilityTemplate::
              control_domain_revision)
      .def_readonly(
          "controller_topology_id",
          &deployment::LogicalStoppableApplicabilityTemplate::
              controller_topology_id)
      .def_readonly(
          "topology_revision",
          &deployment::LogicalStoppableApplicabilityTemplate::topology_revision)
      .def_readonly(
          "containment_granularity",
          &deployment::LogicalStoppableApplicabilityTemplate::
              containment_granularity)
      .def_readonly(
          "disposition",
          &deployment::LogicalStoppableApplicabilityTemplate::disposition)
      .def_readonly(
          "logical_stop_symbol",
          &deployment::LogicalStoppableApplicabilityTemplate::
              logical_stop_symbol)
      .def_readonly(
          "deadline_budget",
          &deployment::LogicalStoppableApplicabilityTemplate::deadline_budget)
      .def_property_readonly(
          "escalation_stages",
          [](const deployment::LogicalStoppableApplicabilityTemplate& value) {
            return immutable_snapshot(value.escalation_stages);
          })
      .def(py::self == py::self)
      .def(py::self != py::self);

  py::tuple stop_applicability_types{2U};
  stop_applicability_types[0] =
      module.attr("LogicalNotStoppableApplicabilityTemplate");
  stop_applicability_types[1] =
      module.attr("LogicalStoppableApplicabilityTemplate");
  module.attr("LogicalStopApplicabilityTemplate") =
      py::module_::import("typing")
          .attr("Union")
          .attr("__getitem__")(stop_applicability_types);

  py::class_<deployment::ApplicabilityQualifiedCellIr>(
      module, "ApplicabilityQualifiedCellIr", py::is_final())
      .def_property_readonly(
          "binding_interfaces",
          [](const deployment::ApplicabilityQualifiedCellIr& value) {
            return immutable_snapshot(value.binding_interfaces());
          })
      .def_property_readonly(
          "capabilities",
          [](const deployment::ApplicabilityQualifiedCellIr& value) {
            return immutable_snapshot(value.capabilities());
          })
      .def_property_readonly(
          "capability_definitions",
          [](const deployment::ApplicabilityQualifiedCellIr& value) {
            return immutable_snapshot(value.capability_definitions());
          })
      .def_property_readonly(
          "command_schemas",
          [](const deployment::ApplicabilityQualifiedCellIr& value) {
            return immutable_snapshot(value.command_schemas());
          })
      .def_property_readonly(
          "composition",
          [](const deployment::ApplicabilityQualifiedCellIr& value) {
            return value.composition();
          })
      .def_property_readonly(
          "control_interface_contracts",
          [](const deployment::ApplicabilityQualifiedCellIr& value) {
            return immutable_snapshot(value.control_interface_contracts());
          })
      .def_property_readonly(
          "controller_topologies",
          [](const deployment::ApplicabilityQualifiedCellIr& value) {
            return immutable_snapshot(value.controller_topologies());
          })
      .def_property_readonly(
          "controllers",
          [](const deployment::ApplicabilityQualifiedCellIr& value) {
            return immutable_snapshot(value.controllers());
          })
      .def_property_readonly(
          "deployment_inputs",
          [](const deployment::ApplicabilityQualifiedCellIr& value) {
            return value.deployment_inputs();
          })
      .def_property_readonly(
          "external_resources",
          [](const deployment::ApplicabilityQualifiedCellIr& value) {
            return immutable_snapshot(value.external_resources());
          })
      .def_property_readonly(
          "provider_endpoints",
          [](const deployment::ApplicabilityQualifiedCellIr& value) {
            return immutable_snapshot(value.provider_endpoints());
          })
      .def_property_readonly(
          "provider_manifests",
          [](const deployment::ApplicabilityQualifiedCellIr& value) {
            return immutable_snapshot(value.provider_manifests());
          })
      .def_property_readonly(
          "routes",
          [](const deployment::ApplicabilityQualifiedCellIr& value) {
            return immutable_snapshot(value.routes());
          })
      .def_property_readonly(
          "runtime_profiles",
          [](const deployment::ApplicabilityQualifiedCellIr& value) {
            return immutable_snapshot(value.runtime_profiles());
          })
      .def_property_readonly(
          "selected_site_overlay_ref",
          [](const deployment::ApplicabilityQualifiedCellIr& value) {
            return value.selected_site_overlay_ref();
          })
      .def_property_readonly(
          "stop_applicability",
          [](const deployment::ApplicabilityQualifiedCellIr& value) {
            return immutable_snapshot(value.stop_applicability());
          })
      .def_property_readonly(
          "stop_evidence_policies",
          [](const deployment::ApplicabilityQualifiedCellIr& value) {
            return immutable_snapshot(value.stop_evidence_policies());
          })
      .def_property_readonly(
          "assistant_settings",
          [](const deployment::ApplicabilityQualifiedCellIr& value) {
            return value.assistant_settings();
          });

  py::class_<deployment::CompilerQualifiedCellIr>(
      module, "CompilerQualifiedCellIr", py::is_final())
      .def_property_readonly(
          "applicability",
          [](const deployment::CompilerQualifiedCellIr& value) {
            return value.applicability();
          })
      .def_property_readonly(
          "catalog_record_refs",
          [](const deployment::CompilerQualifiedCellIr& value) {
            return immutable_snapshot(value.catalog_record_refs());
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
      "compose_cell_occurrences",
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
        return deployment::compose_cell_occurrences(
            graph,
            std::span<const deployment::SourceVerifiedModelAsset>{
                bounded_assets},
            std::span<const deployment::QualifiedComponentType>{
                bounded_types});
      },
      py::arg("graph"), py::arg("model_assets"),
      py::arg("component_types"));

  module.def(
      "qualify_cell_applicability",
      [](const deployment::CompositionQualifiedCellIr& composition,
         const deployment::RevisionVerifiedDeploymentApplicabilityInputs&
             deployment_inputs,
         const py::iterable& runtime_profiles,
         const py::iterable& runtime_policies,
         const py::iterable& command_schemas,
         const py::iterable& capability_definitions,
         const py::iterable& control_interface_contracts,
         const py::iterable& controller_topologies,
         const py::iterable& provider_manifests,
         const py::iterable& stop_evidence_policies,
         const py::iterable& value_schemas,
         const py::iterable& commissioning_evidence_refs,
         const deployment::QualifiedSelectedSiteOverlay&
             selected_site_overlay) {
        auto bounded_runtime_profiles =
            bounded_qualified_closure_from_python<
                deployment::QualifiedRuntimeProfile>(
                runtime_profiles, "runtime_profiles",
                "QualifiedRuntimeProfile");
        auto bounded_runtime_policies =
            bounded_qualified_closure_from_python<
                deployment::QualifiedRuntimePolicy>(
                runtime_policies, "runtime_policies",
                "QualifiedRuntimePolicy");
        auto bounded_command_schemas =
            bounded_qualified_closure_from_python<
                deployment::QualifiedCommandSchema>(
                command_schemas, "command_schemas",
                "QualifiedCommandSchema");
        auto bounded_capability_definitions =
            bounded_qualified_closure_from_python<
                deployment::QualifiedCapabilityDefinition>(
                capability_definitions, "capability_definitions",
                "QualifiedCapabilityDefinition");
        auto bounded_control_interface_contracts =
            bounded_qualified_closure_from_python<
                deployment::QualifiedControlInterfaceContract>(
                control_interface_contracts,
                "control_interface_contracts",
                "QualifiedControlInterfaceContract");
        auto bounded_controller_topologies =
            bounded_qualified_closure_from_python<
                deployment::QualifiedControllerTopology>(
                controller_topologies, "controller_topologies",
                "QualifiedControllerTopology");
        auto bounded_provider_manifests =
            bounded_qualified_closure_from_python<
                deployment::QualifiedProviderManifest>(
                provider_manifests, "provider_manifests",
                "QualifiedProviderManifest");
        auto bounded_stop_evidence_policies =
            bounded_qualified_closure_from_python<
                deployment::QualifiedStopEvidencePolicy>(
                stop_evidence_policies, "stop_evidence_policies",
                "QualifiedStopEvidencePolicy");
        auto bounded_value_schemas =
            bounded_qualified_closure_from_python<
                deployment::BuildQualifiedValueSchemaEntry>(
                value_schemas, "value_schemas",
                "BuildQualifiedValueSchemaEntry");
        auto bounded_commissioning_evidence_refs =
            bounded_qualified_closure_from_python<
                deployment::QualifiedCommissioningEvidenceRef>(
                commissioning_evidence_refs,
                "commissioning_evidence_refs",
                "QualifiedCommissioningEvidenceRef");
        py::gil_scoped_release release;
        return deployment::qualify_cell_applicability(
            composition, deployment_inputs,
            std::span<const deployment::QualifiedRuntimeProfile>{
                bounded_runtime_profiles},
            std::span<const deployment::QualifiedRuntimePolicy>{
                bounded_runtime_policies},
            std::span<const deployment::QualifiedCommandSchema>{
                bounded_command_schemas},
            std::span<const deployment::QualifiedCapabilityDefinition>{
                bounded_capability_definitions},
            std::span<const deployment::QualifiedControlInterfaceContract>{
                bounded_control_interface_contracts},
            std::span<const deployment::QualifiedControllerTopology>{
                bounded_controller_topologies},
            std::span<const deployment::QualifiedProviderManifest>{
                bounded_provider_manifests},
            std::span<const deployment::QualifiedStopEvidencePolicy>{
                bounded_stop_evidence_policies},
            std::span<const deployment::BuildQualifiedValueSchemaEntry>{
                bounded_value_schemas},
            std::span<const deployment::QualifiedCommissioningEvidenceRef>{
                bounded_commissioning_evidence_refs},
            selected_site_overlay);
      },
      py::arg("composition"), py::arg("deployment_inputs"),
      py::arg("runtime_profiles"), py::arg("runtime_policies"),
      py::arg("command_schemas"),
      py::arg("capability_definitions"),
      py::arg("control_interface_contracts"),
      py::arg("controller_topologies"), py::arg("provider_manifests"),
      py::arg("stop_evidence_policies"),
      py::arg("value_schemas"),
      py::arg("commissioning_evidence_refs"),
      py::arg("selected_site_overlay"));

  module.def("compile_cell_ir", &deployment::compile_cell_ir,
             py::arg("applicability"),
             py::call_guard<py::gil_scoped_release>());

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
  module.attr("MAX_COMPOSITION_SYMBOLS") =
      py::int_(deployment::kMaxCompositionSymbols);
  module.attr("MAX_COMPOSITION_REFERENCES") =
      py::int_(deployment::kMaxCompositionReferences);
  module.attr("MAX_COMPOSITION_PATH_SEGMENTS") =
      py::int_(deployment::kMaxCompositionPathSegments);
  module.attr("MAX_COMPOSITION_COORDINATE_PROJECTIONS") =
      py::int_(deployment::kMaxCompositionCoordinateProjections);
  module.attr("MAX_COMPOSITION_FRAME_RESOLUTIONS") =
      py::int_(deployment::kMaxCompositionFrameResolutions);
  module.attr("MAX_COMPOSITION_TARGET_PROJECTION_ROWS") =
      py::int_(deployment::kMaxCompositionTargetProjectionRows);
  module.attr("MAX_COMPOSITION_FLATTENED_XML_BYTES") =
      py::int_(deployment::kMaxCompositionFlattenedXmlBytes);
  module.attr("MAX_EXPANDED_CONTROL_INTERFACE_CLAIMS_PER_DIRECTION") =
      py::int_(deployment::kMaxExpandedControlInterfaceClaimsPerDirection);
  module.attr("MAX_CELL_EXPANDED_CONTROL_INTERFACE_CLAIMS_PER_DIRECTION") =
      py::int_(
          deployment::kMaxCellExpandedControlInterfaceClaimsPerDirection);
  module.attr("MAX_CELL_LOGICAL_APPLICABILITY_ROWS") =
      py::int_(deployment::kMaxCellLogicalApplicabilityRows);
}

}  // namespace manyforge::core::python
