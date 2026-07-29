#include <pybind11/operators.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <array>
#include <manyforge_core/types/catalog.hpp>
#include <manyforge_core/types/capability_definition.hpp>
#include <optional>
#include <string>
#include <utility>

#include "binding_registrations.hpp"

namespace py = pybind11;

namespace manyforge::core::python {
namespace {

template <typename Record>
void bind_catalog_record_ref(py::module_& module, const char* python_name) {
  using Id = typename Record::IdType;
  using Revision = typename Record::RevisionType;

  py::class_<Record>(module, python_name, py::is_final())
      .def(py::init([](Id record_id, Revision record_revision, SchemaId schema_id,
                       SchemaRevision schema_revision, ContentDigest content_digest,
                       ContentAddressedStoreKey content_addressed_store_key) {
             return Record{
                 std::move(record_id),      std::move(record_revision),
                 std::move(schema_id),      std::move(schema_revision),
                 std::move(content_digest), std::move(content_addressed_store_key),
             };
           }),
           py::arg("record_id"), py::arg("record_revision"), py::arg("schema_id"),
           py::arg("schema_revision"), py::arg("content_digest"),
           py::arg("content_addressed_store_key"))
      .def_readonly("record_id", &Record::record_id)
      .def_readonly("record_revision", &Record::record_revision)
      .def_readonly("schema_id", &Record::schema_id)
      .def_readonly("schema_revision", &Record::schema_revision)
      .def_readonly("content_digest", &Record::content_digest)
      .def_readonly("content_addressed_store_key", &Record::content_addressed_store_key)
      .def_property_readonly("record_kind", [](const Record&) { return Record::kRecordKind; })
      .def(
          "__eq__", [](const Record& lhs, const Record& rhs) { return lhs == rhs; },
          py::is_operator())
      .def(
          "__ne__", [](const Record& lhs, const Record& rhs) { return lhs != rhs; },
          py::is_operator())
      .def("__repr__", [python_name](const Record& record) {
        return py::str("{}(record_id={}, record_revision={})")
            .format(python_name, py::repr(py::str(record.record_id.value())),
                    py::repr(py::str(record.record_revision.value())));
      });
}

template <typename Record>
void bind_catalog_record_ref_helpers(py::module_& module) {
  module.def(
      "catalog_record_type_descriptor",
      [](const Record&) {
        return *manyforge::core::catalog_record_type_descriptor(Record::kRecordKind);
      },
      py::arg("record"));
  module.def(
      "catalog_record_kind", [](const Record&) { return Record::kRecordKind; }, py::arg("record"));
  module.def(
      "catalog_record_id_value", [](const Record& record) { return record.record_id.value(); },
      py::arg("record"));
  module.def(
      "catalog_record_revision_value",
      [](const Record& record) { return record.record_revision.value(); }, py::arg("record"));
  module.def(
      "catalog_record_ref_matches_registry_family",
      [](const Record& record) {
        const auto* descriptor =
            manyforge::core::catalog_record_type_descriptor(Record::kRecordKind);
        return descriptor != nullptr && record.schema_id.value() == descriptor->schema_family;
      },
      py::arg("record"));
}

}  // namespace

void register_catalog_record_bindings(py::module_& module) {
  py::enum_<CapabilityScope>(module, "CapabilityScope")
      .value("kCellGlobal", CapabilityScope::kCellGlobal)
      .value("kControlDomain", CapabilityScope::kControlDomain)
      .value("kTargetBinding", CapabilityScope::kTargetBinding);

  py::enum_<CapabilityProviderBindingKind>(module,
                                           "CapabilityProviderBindingKind")
      .value("kCapabilityProviderBinding",
             CapabilityProviderBindingKind::kCapabilityProviderBinding)
      .value("kControlBinding",
             CapabilityProviderBindingKind::kControlBinding);

  py::class_<ValueSchemaRefDTO>(module, "ValueSchemaRefDTO", py::is_final())
      .def(py::init([](SchemaId schema_id, SchemaRevision schema_revision) {
             return ValueSchemaRefDTO{std::move(schema_id),
                                      std::move(schema_revision)};
           }),
           py::arg("schema_id"), py::arg("schema_revision"))
      .def_readonly("schema_id", &ValueSchemaRefDTO::schema_id)
      .def_readonly("schema_revision", &ValueSchemaRefDTO::schema_revision)
      .def(
          "__eq__",
          [](const ValueSchemaRefDTO& lhs, const ValueSchemaRefDTO& rhs) {
            return lhs == rhs;
          },
          py::is_operator())
      .def(
          "__ne__",
          [](const ValueSchemaRefDTO& lhs, const ValueSchemaRefDTO& rhs) {
            return lhs != rhs;
          },
          py::is_operator());

  py::class_<CapabilityDefinitionDTO>(module, "CapabilityDefinitionDTO",
                                      py::is_final())
      .def(py::init([](CapabilityId capability_id,
                       CapabilityDefinitionRevision revision,
                       CapabilityScope scope,
                       CapabilityProviderBindingKind provider_binding_kind,
                       std::optional<ValueSchemaRefDTO> schema_ref) {
             return CapabilityDefinitionDTO{
                 std::move(capability_id), std::move(revision), scope,
                 provider_binding_kind, std::move(schema_ref)};
           }),
           py::arg("capability_id"),
           py::arg("capability_definition_revision"), py::arg("scope"),
           py::arg("provider_binding_kind"),
           py::arg("requirement_parameters_schema_ref") = std::nullopt)
      .def_readonly("capability_id", &CapabilityDefinitionDTO::capability_id)
      .def_readonly("capability_definition_revision",
                    &CapabilityDefinitionDTO::capability_definition_revision)
      .def_readonly("scope", &CapabilityDefinitionDTO::scope)
      .def_readonly("provider_binding_kind",
                    &CapabilityDefinitionDTO::provider_binding_kind)
      .def_readonly("requirement_parameters_schema_ref",
                    &CapabilityDefinitionDTO::requirement_parameters_schema_ref)
      .def(
          "__eq__",
          [](const CapabilityDefinitionDTO& lhs,
             const CapabilityDefinitionDTO& rhs) { return lhs == rhs; },
          py::is_operator())
      .def(
          "__ne__",
          [](const CapabilityDefinitionDTO& lhs,
             const CapabilityDefinitionDTO& rhs) { return lhs != rhs; },
          py::is_operator());

  py::class_<ValueSchemaRefDescriptor>(module, "ValueSchemaRefDescriptor",
                                       py::is_final())
      .def_property_readonly(
          "schema_id", [](const ValueSchemaRefDescriptor& value) {
            return std::string(value.schema_id);
          })
      .def_property_readonly(
          "schema_revision", [](const ValueSchemaRefDescriptor& value) {
            return std::string(value.schema_revision);
          });

  py::class_<CapabilityDefinitionDescriptor>(
      module, "CapabilityDefinitionDescriptor", py::is_final())
      .def_property_readonly(
          "capability_id", [](const CapabilityDefinitionDescriptor& value) {
            return std::string(value.capability_id);
          })
      .def_property_readonly(
          "capability_definition_revision",
          [](const CapabilityDefinitionDescriptor& value) {
            return std::string(value.capability_definition_revision);
          })
      .def_readonly("scope", &CapabilityDefinitionDescriptor::scope)
      .def_readonly("provider_binding_kind",
                    &CapabilityDefinitionDescriptor::provider_binding_kind)
      .def_readonly(
          "requirement_parameters_schema_ref",
          &CapabilityDefinitionDescriptor::requirement_parameters_schema_ref);

  module.attr("CAPABILITY_DEFINITION_API_VERSION") =
      std::string(kCapabilityDefinitionApiVersion);
  module.attr("BUILTIN_CAPABILITY_DEFINITION_REGISTRY_DIGEST") =
      std::string(kBuiltinCapabilityDefinitionRegistryDigest);
  py::tuple capability_registry{kBuiltinCapabilityDefinitionRegistry.size()};
  for (std::size_t index = 0U;
       index < kBuiltinCapabilityDefinitionRegistry.size(); ++index) {
    capability_registry[index] =
        py::cast(kBuiltinCapabilityDefinitionRegistry[index]);
  }
  module.attr("BUILTIN_CAPABILITY_DEFINITION_REGISTRY") =
      capability_registry;
  module.def(
      "find_builtin_capability_definition",
      [](std::string_view capability_id)
          -> std::optional<CapabilityDefinitionDescriptor> {
        const auto* found =
            manyforge::core::find_builtin_capability_definition(capability_id);
        return found == nullptr
                   ? std::nullopt
                   : std::optional<CapabilityDefinitionDescriptor>(*found);
      },
      py::arg("capability_id"));
  module.def("materialize_capability_definition",
             &manyforge::core::materialize_capability_definition,
             py::arg("descriptor"));
  module.def(
      "capability_scope_to_string",
      [](CapabilityScope scope) { return std::string(to_string(scope)); },
      py::arg("scope"));
  module.def(
      "capability_provider_binding_kind_to_string",
      [](CapabilityProviderBindingKind kind) {
        return std::string(to_string(kind));
      },
      py::arg("kind"));

  py::enum_<CatalogRecordKind>(module, "CatalogRecordKind")
      .value("kModelAsset", CatalogRecordKind::kModelAsset)
      .value("kRuntimeProfile", CatalogRecordKind::kRuntimeProfile)
      .value("kComponentType", CatalogRecordKind::kComponentType)
      .value("kControlInterfaceContract", CatalogRecordKind::kControlInterfaceContract)
      .value("kControllerTopology", CatalogRecordKind::kControllerTopology)
      .value("kProviderManifest", CatalogRecordKind::kProviderManifest)
      .value("kCommandSchema", CatalogRecordKind::kCommandSchema)
      .value("kCapabilityDefinition", CatalogRecordKind::kCapabilityDefinition)
      .value("kContactPolicy", CatalogRecordKind::kContactPolicy)
      .value("kStopEvidencePolicy", CatalogRecordKind::kStopEvidencePolicy)
      .value("kRuntimePolicy", CatalogRecordKind::kRuntimePolicy);

  py::class_<CatalogRecordTypeDescriptor>(module, "CatalogRecordTypeDescriptor", py::is_final())
      .def_readonly("record_kind", &CatalogRecordTypeDescriptor::record_kind)
      .def_property_readonly("serialized_kind",
                             [](const CatalogRecordTypeDescriptor& descriptor) {
                               return std::string{descriptor.serialized_kind};
                             })
      .def_property_readonly("own_id_field",
                             [](const CatalogRecordTypeDescriptor& descriptor) {
                               return std::string{descriptor.own_id_field};
                             })
      .def_property_readonly("own_revision_field",
                             [](const CatalogRecordTypeDescriptor& descriptor) {
                               return std::string{descriptor.own_revision_field};
                             })
      .def_property_readonly("schema_family",
                             [](const CatalogRecordTypeDescriptor& descriptor) {
                               return std::string{descriptor.schema_family};
                             })
      .def(
          "__eq__",
          [](const CatalogRecordTypeDescriptor& lhs, const CatalogRecordTypeDescriptor& rhs) {
            return lhs == rhs;
          },
          py::is_operator())
      .def(
          "__ne__",
          [](const CatalogRecordTypeDescriptor& lhs, const CatalogRecordTypeDescriptor& rhs) {
            return lhs != rhs;
          },
          py::is_operator());

  bind_catalog_record_ref<ModelAssetCatalogRecordRefDTO>(module, "ModelAssetCatalogRecordRefDTO");
  bind_catalog_record_ref<RuntimeProfileCatalogRecordRefDTO>(module,
                                                             "RuntimeProfileCatalogRecordRefDTO");
  bind_catalog_record_ref<ComponentTypeCatalogRecordRefDTO>(module,
                                                            "ComponentTypeCatalogRecordRefDTO");
  bind_catalog_record_ref<ControlInterfaceContractCatalogRecordRefDTO>(
      module, "ControlInterfaceContractCatalogRecordRefDTO");
  bind_catalog_record_ref<ControllerTopologyCatalogRecordRefDTO>(
      module, "ControllerTopologyCatalogRecordRefDTO");
  bind_catalog_record_ref<ProviderManifestCatalogRecordRefDTO>(
      module, "ProviderManifestCatalogRecordRefDTO");
  bind_catalog_record_ref<CommandSchemaCatalogRecordRefDTO>(module,
                                                            "CommandSchemaCatalogRecordRefDTO");
  bind_catalog_record_ref<CapabilityDefinitionCatalogRecordRefDTO>(
      module, "CapabilityDefinitionCatalogRecordRefDTO");
  bind_catalog_record_ref<ContactPolicyCatalogRecordRefDTO>(module,
                                                            "ContactPolicyCatalogRecordRefDTO");
  bind_catalog_record_ref<StopEvidencePolicyCatalogRecordRefDTO>(
      module, "StopEvidencePolicyCatalogRecordRefDTO");
  bind_catalog_record_ref<RuntimePolicyCatalogRecordRefDTO>(module,
                                                            "RuntimePolicyCatalogRecordRefDTO");

  constexpr std::array<const char*, 11> kRecordRefTypeNames{{
      "ModelAssetCatalogRecordRefDTO",
      "RuntimeProfileCatalogRecordRefDTO",
      "ComponentTypeCatalogRecordRefDTO",
      "ControlInterfaceContractCatalogRecordRefDTO",
      "ControllerTopologyCatalogRecordRefDTO",
      "ProviderManifestCatalogRecordRefDTO",
      "CommandSchemaCatalogRecordRefDTO",
      "CapabilityDefinitionCatalogRecordRefDTO",
      "ContactPolicyCatalogRecordRefDTO",
      "StopEvidencePolicyCatalogRecordRefDTO",
      "RuntimePolicyCatalogRecordRefDTO",
  }};
  py::tuple record_ref_types{kRecordRefTypeNames.size()};
  for (std::size_t index = 0; index < kRecordRefTypeNames.size(); ++index) {
    record_ref_types[index] = module.attr(kRecordRefTypeNames[index]);
  }
  module.attr("CatalogRecordRefTypes") = record_ref_types;
  module.attr("CatalogRecordRefDTO") =
      py::module_::import("typing").attr("Union").attr("__getitem__")(record_ref_types);

  py::tuple registry{kCatalogRecordTypeRegistry.size()};
  for (std::size_t index = 0; index < kCatalogRecordTypeRegistry.size(); ++index) {
    registry[index] = py::cast(kCatalogRecordTypeRegistry[index]);
  }
  module.attr("CATALOG_RECORD_TYPE_REGISTRY") = registry;

  module.def(
      "catalog_record_type_descriptor",
      [](CatalogRecordKind kind) {
        const auto* descriptor = manyforge::core::catalog_record_type_descriptor(kind);
        if (descriptor == nullptr) {
          throw py::value_error("unknown CatalogRecordKind");
        }
        return *descriptor;
      },
      py::arg("record_kind"));

  bind_catalog_record_ref_helpers<ModelAssetCatalogRecordRefDTO>(module);
  bind_catalog_record_ref_helpers<RuntimeProfileCatalogRecordRefDTO>(module);
  bind_catalog_record_ref_helpers<ComponentTypeCatalogRecordRefDTO>(module);
  bind_catalog_record_ref_helpers<ControlInterfaceContractCatalogRecordRefDTO>(module);
  bind_catalog_record_ref_helpers<ControllerTopologyCatalogRecordRefDTO>(module);
  bind_catalog_record_ref_helpers<ProviderManifestCatalogRecordRefDTO>(module);
  bind_catalog_record_ref_helpers<CommandSchemaCatalogRecordRefDTO>(module);
  bind_catalog_record_ref_helpers<CapabilityDefinitionCatalogRecordRefDTO>(module);
  bind_catalog_record_ref_helpers<ContactPolicyCatalogRecordRefDTO>(module);
  bind_catalog_record_ref_helpers<StopEvidencePolicyCatalogRecordRefDTO>(module);
  bind_catalog_record_ref_helpers<RuntimePolicyCatalogRecordRefDTO>(module);
}

}  // namespace manyforge::core::python
