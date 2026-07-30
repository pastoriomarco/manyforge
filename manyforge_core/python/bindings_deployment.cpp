#include <pybind11/pybind11.h>

#include <manyforge_deployment/artifact_codec.hpp>
#include <manyforge_deployment/capability_definition_codec.hpp>
#include <manyforge_deployment/canonical_value.hpp>
#include <manyforge_deployment/cell_graph_codec.hpp>
#include <manyforge_deployment/closed_schema.hpp>
#include <manyforge_deployment/control_interface_contract_codec.hpp>
#include <manyforge_deployment/model_asset_codec.hpp>
#include <manyforge_deployment/provider_manifest_codec.hpp>
#include <manyforge_deployment/runtime_profile_codec.hpp>
#include <manyforge_deployment/stop_evidence_policy_codec.hpp>
#include <string>
#include <string_view>

#include "binding_registrations.hpp"

namespace py = pybind11;

namespace manyforge::core::python {

void register_deployment_candidate_bindings(py::module_& module) {
  using deployment::IntrinsicallyValidatedCellGraphCandidate;
  using deployment::ParsedArtifactCandidate;
  using deployment::TypedCellGraphCandidate;
  using deployment::UnqualifiedSchemaNormalizedArtifactCandidate;

  py::register_exception<deployment::ArtifactCodecError>(module, "ArtifactCodecError",
                                                         PyExc_ValueError);
  py::register_exception<deployment::CellGraphCodecError>(module, "CellGraphCodecError",
                                                          PyExc_ValueError);
  py::register_exception<deployment::ClosedSchemaError>(module, "ClosedSchemaError",
                                                        PyExc_ValueError);
  py::register_exception<deployment::CapabilityDefinitionCodecError>(
      module, "CapabilityDefinitionCodecError", PyExc_ValueError);
  py::register_exception<deployment::ControlInterfaceContractCodecError>(
      module, "ControlInterfaceContractCodecError", PyExc_ValueError);
  py::register_exception<deployment::ModelAssetCodecError>(
      module, "ModelAssetCodecError", PyExc_ValueError);
  py::register_exception<deployment::ProviderManifestCodecError>(
      module, "ProviderManifestCodecError", PyExc_ValueError);
  py::register_exception<deployment::RuntimeProfileCodecError>(
      module, "RuntimeProfileCodecError", PyExc_ValueError);
  py::register_exception<deployment::StopEvidencePolicyCodecError>(
      module, "StopEvidencePolicyCodecError", PyExc_ValueError);

  py::class_<ParsedArtifactCandidate>(module, "ParsedArtifactCandidate", py::is_final())
      .def_property_readonly("canonical_json", [](const ParsedArtifactCandidate& candidate) {
        py::gil_scoped_release release;
        return deployment::canonical_json(candidate.root());
      });

  py::class_<TypedCellGraphCandidate>(module, "TypedCellGraphCandidate", py::is_final())
      .def_property_readonly("graph",
                             [](const TypedCellGraphCandidate& candidate) {
                               py::gil_scoped_release release;
                               return candidate.graph();
                             })
      .def_property_readonly(
          "structurally_normalized_canonical_json", [](const TypedCellGraphCandidate& candidate) {
            py::gil_scoped_release release;
            return deployment::canonical_json(candidate.structurally_normalized_root());
          });

  py::class_<IntrinsicallyValidatedCellGraphCandidate>(
      module, "IntrinsicallyValidatedCellGraphCandidate", py::is_final())
      .def_property_readonly("graph",
                             [](const IntrinsicallyValidatedCellGraphCandidate& candidate) {
                               py::gil_scoped_release release;
                               return candidate.graph();
                             })
      .def_property_readonly(
          "structurally_normalized_canonical_json",
          [](const IntrinsicallyValidatedCellGraphCandidate& candidate) {
            py::gil_scoped_release release;
            return deployment::canonical_json(candidate.structurally_normalized_root());
          });

  py::enum_<deployment::ClosedSchemaPurpose>(module, "ClosedSchemaPurpose")
      .value("kArtifactDocument", deployment::ClosedSchemaPurpose::kArtifactDocument)
      .value("kValue", deployment::ClosedSchemaPurpose::kValue);

  py::enum_<deployment::ClosedSchemaRootShape>(module, "ClosedSchemaRootShape")
      .value("kScalar", deployment::ClosedSchemaRootShape::kScalar)
      .value("kObject", deployment::ClosedSchemaRootShape::kObject)
      .value("kArray", deployment::ClosedSchemaRootShape::kArray)
      .value("kTaggedUnion", deployment::ClosedSchemaRootShape::kTaggedUnion);

  py::enum_<deployment::CatalogSchemaQualification>(module, "CatalogSchemaQualification")
      .value("kUnqualified", deployment::CatalogSchemaQualification::kUnqualified)
      .value("kQualified", deployment::CatalogSchemaQualification::kQualified);

  py::class_<deployment::CompiledClosedSchema>(module, "CompiledClosedSchema", py::is_final())
      .def_property_readonly("schema_id", &deployment::CompiledClosedSchema::schema_id)
      .def_property_readonly("schema_revision",
                             &deployment::CompiledClosedSchema::schema_revision)
      .def_property_readonly("schema_purpose",
                             &deployment::CompiledClosedSchema::schema_purpose)
      .def_property_readonly("root_shape", &deployment::CompiledClosedSchema::root_shape)
      .def_property_readonly(
          "normalized_descriptor_canonical_json",
          [](const deployment::CompiledClosedSchema& schema) {
            py::gil_scoped_release release;
            return deployment::canonical_json(schema.normalized_descriptor());
          });

  py::class_<UnqualifiedSchemaNormalizedArtifactCandidate>(
      module, "UnqualifiedSchemaNormalizedArtifactCandidate", py::is_final())
      .def_property_readonly(
          "schema_id", &UnqualifiedSchemaNormalizedArtifactCandidate::schema_id)
      .def_property_readonly(
          "schema_revision",
          &UnqualifiedSchemaNormalizedArtifactCandidate::schema_revision)
      .def_property_readonly(
          "schema_purpose",
          &UnqualifiedSchemaNormalizedArtifactCandidate::schema_purpose)
      .def_property_readonly("normalized_canonical_json",
                             [](const UnqualifiedSchemaNormalizedArtifactCandidate&
                                    artifact) {
                               py::gil_scoped_release release;
                               return deployment::canonical_json(artifact.normalized_root());
                             });

  py::class_<deployment::BuildCatalogSchemaEntry>(module, "BuildCatalogSchemaEntry",
                                                   py::is_final())
      .def_property_readonly("record_kind",
                             &deployment::BuildCatalogSchemaEntry::record_kind)
      .def_property_readonly("schema_id", &deployment::BuildCatalogSchemaEntry::schema_id)
      .def_property_readonly("qualification",
                             &deployment::BuildCatalogSchemaEntry::qualification)
      .def_property_readonly(
          "relative_path",
          [](const deployment::BuildCatalogSchemaEntry& entry) {
            return std::string(entry.relative_path());
          })
      .def_property_readonly(
          "compiled_schema",
          [](const deployment::BuildCatalogSchemaEntry& entry) -> py::object {
            const auto* schema = entry.compiled_schema();
            if (schema == nullptr) {
              return py::none();
            }
            return py::cast(schema, py::return_value_policy::reference);
          });

  py::class_<deployment::BuildQualifiedValueSchemaEntry>(
      module, "BuildQualifiedValueSchemaEntry", py::is_final())
      .def_property_readonly("schema_id",
                             &deployment::BuildQualifiedValueSchemaEntry::schema_id)
      .def_property_readonly("schema_revision",
                             &deployment::BuildQualifiedValueSchemaEntry::schema_revision)
      .def_property_readonly(
          "relative_path",
          [](const deployment::BuildQualifiedValueSchemaEntry& entry) {
            return std::string(entry.relative_path());
          })
      .def_property_readonly(
          "compiled_schema",
          [](const deployment::BuildQualifiedValueSchemaEntry& entry) {
            return &entry.compiled_schema();
          },
          py::return_value_policy::reference);

  py::class_<deployment::BuildQualifiedClosedSchemaRegistry>(
      module, "BuildQualifiedClosedSchemaRegistry", py::is_final())
      .def_property_readonly(
          "catalog_schemas",
          [](const deployment::BuildQualifiedClosedSchemaRegistry& registry) {
            const auto entries = registry.catalog_schemas();
            py::tuple snapshot{entries.size()};
            for (std::size_t index = 0; index < entries.size(); ++index) {
              snapshot[index] =
                  py::cast(&entries[index], py::return_value_policy::reference);
            }
            return snapshot;
          })
      .def_property_readonly(
          "value_schemas",
          [](const deployment::BuildQualifiedClosedSchemaRegistry& registry) {
            const auto entries = registry.value_schemas();
            py::tuple snapshot{entries.size()};
            for (std::size_t index = 0; index < entries.size(); ++index) {
              snapshot[index] =
                  py::cast(&entries[index], py::return_value_policy::reference);
            }
            return snapshot;
          })
      .def_property_readonly(
          "normalized_projection_canonical_json",
          [](const deployment::BuildQualifiedClosedSchemaRegistry& registry) {
            py::gil_scoped_release release;
            return deployment::canonical_json(registry.normalized_projection());
          })
      .def_property_readonly(
          "qualification_registry_digest",
          &deployment::BuildQualifiedClosedSchemaRegistry::
              qualification_registry_digest);

  py::class_<deployment::QualifiedCapabilityDefinition>(
      module, "QualifiedCapabilityDefinition", py::is_final())
      .def_property_readonly(
          "value", &deployment::QualifiedCapabilityDefinition::value)
      .def_property_readonly(
          "normalized_canonical_json",
          [](const deployment::QualifiedCapabilityDefinition& definition) {
            py::gil_scoped_release release;
            return deployment::canonical_json(definition.normalized_root());
          });

  py::class_<deployment::QualifiedControlInterfaceContract>(
      module, "QualifiedControlInterfaceContract", py::is_final())
      .def_property_readonly(
          "value", &deployment::QualifiedControlInterfaceContract::value)
      .def_property_readonly(
          "normalized_canonical_json",
          [](const deployment::QualifiedControlInterfaceContract& contract) {
            py::gil_scoped_release release;
            return deployment::canonical_json(contract.normalized_root());
          });

  py::class_<deployment::QualifiedModelAsset>(
      module, "QualifiedModelAsset", py::is_final())
      .def_property_readonly("value",
                             &deployment::QualifiedModelAsset::value)
      .def_property_readonly(
          "normalized_canonical_json",
          [](const deployment::QualifiedModelAsset& asset) {
            py::gil_scoped_release release;
            return deployment::canonical_json(asset.normalized_root());
          });

  py::class_<deployment::QualifiedProviderManifest>(
      module, "QualifiedProviderManifest", py::is_final())
      .def_property_readonly(
          "value", &deployment::QualifiedProviderManifest::value)
      .def_property_readonly(
          "normalized_canonical_json",
          [](const deployment::QualifiedProviderManifest& manifest) {
            py::gil_scoped_release release;
            return deployment::canonical_json(manifest.normalized_root());
          });

  py::class_<deployment::QualifiedRuntimeProfile>(
      module, "QualifiedRuntimeProfile", py::is_final())
      .def_property_readonly(
          "value", &deployment::QualifiedRuntimeProfile::value)
      .def_property_readonly(
          "normalized_canonical_json",
          [](const deployment::QualifiedRuntimeProfile& profile) {
            py::gil_scoped_release release;
            return deployment::canonical_json(profile.normalized_root());
          });

  py::class_<deployment::QualifiedStopEvidencePolicy>(
      module, "QualifiedStopEvidencePolicy", py::is_final())
      .def_property_readonly(
          "value", &deployment::QualifiedStopEvidencePolicy::value)
      .def_property_readonly(
          "normalized_canonical_json",
          [](const deployment::QualifiedStopEvidencePolicy& policy) {
            py::gil_scoped_release release;
            return deployment::canonical_json(policy.normalized_root());
          });

  module.def(
      "parse_strict_json_model_yaml",
      [](const std::string& yaml_bytes) {
        return deployment::parse_strict_json_model_yaml(yaml_bytes);
      },
      py::arg("yaml_bytes"), py::call_guard<py::gil_scoped_release>());
  module.def("decode_cell_graph_candidate", &deployment::decode_cell_graph_candidate,
             py::arg("parsed"), py::call_guard<py::gil_scoped_release>());
  module.def("validate_intrinsic_cell_graph_candidate",
             &deployment::validate_intrinsic_cell_graph_candidate, py::arg("candidate"),
             py::call_guard<py::gil_scoped_release>());
  module.def("compile_closed_schema_descriptor",
             &deployment::compile_closed_schema_descriptor, py::arg("candidate"),
             py::call_guard<py::gil_scoped_release>());
  module.def("validate_and_normalize_unqualified_artifact_candidate",
             &deployment::validate_and_normalize_unqualified_artifact_candidate,
             py::arg("schema"), py::arg("candidate"),
             py::call_guard<py::gil_scoped_release>());
  module.def("resolve_and_qualify_capability_definition",
             &deployment::resolve_and_qualify_capability_definition,
             py::arg("reference"), py::arg("resolved_store_key"),
             py::arg("stored_bytes"),
             py::call_guard<py::gil_scoped_release>());
  module.def("resolve_and_qualify_control_interface_contract",
             &deployment::resolve_and_qualify_control_interface_contract,
             py::arg("reference"), py::arg("resolved_store_key"),
             py::arg("stored_bytes"),
             py::call_guard<py::gil_scoped_release>());
  module.def("resolve_and_qualify_model_asset",
             &deployment::resolve_and_qualify_model_asset,
             py::arg("reference"), py::arg("resolved_store_key"),
             py::arg("stored_bytes"),
             py::call_guard<py::gil_scoped_release>());
  module.def("resolve_and_qualify_provider_manifest",
             &deployment::resolve_and_qualify_provider_manifest,
             py::arg("reference"), py::arg("resolved_store_key"),
             py::arg("stored_bytes"),
             py::call_guard<py::gil_scoped_release>());
  module.def("resolve_and_qualify_runtime_profile",
             &deployment::resolve_and_qualify_runtime_profile,
             py::arg("reference"), py::arg("resolved_store_key"),
             py::arg("stored_bytes"),
             py::call_guard<py::gil_scoped_release>());
  module.def("resolve_and_qualify_stop_evidence_policy",
             &deployment::resolve_and_qualify_stop_evidence_policy,
             py::arg("reference"), py::arg("resolved_store_key"),
             py::arg("stored_bytes"),
             py::call_guard<py::gil_scoped_release>());
  module.def(
      "build_qualified_closed_schema_registry",
      []() -> const deployment::BuildQualifiedClosedSchemaRegistry& {
        return deployment::build_qualified_closed_schema_registry();
      },
      py::return_value_policy::reference);
  module.attr("CLOSED_SCHEMA_API_VERSION") =
      py::str(deployment::kClosedSchemaApiVersion);
  module.attr("CLOSED_SCHEMA_BOOTSTRAP_CONTRACT_DIGEST") =
      py::str(deployment::kClosedSchemaBootstrapContractDigest);
}

}  // namespace manyforge::core::python
