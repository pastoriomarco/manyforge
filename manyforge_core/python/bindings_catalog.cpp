#include <pybind11/operators.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <array>
#include <manyforge_core/stop_timing.hpp>
#include <manyforge_core/types/capability_definition.hpp>
#include <manyforge_core/types/catalog.hpp>
#include <manyforge_core/types/control_interface_contract.hpp>
#include <manyforge_core/types/controller_topology.hpp>
#include <manyforge_core/types/model_asset.hpp>
#include <manyforge_core/types/provider_manifest.hpp>
#include <manyforge_core/types/runtime_profile.hpp>
#include <manyforge_core/types/stop_control.hpp>
#include <manyforge_core/types/stop_evidence_policy.hpp>
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
  py::enum_<ModelAssetInstanceCardinality>(module, "ModelAssetInstanceCardinality")
      .value("kOnePerCell", ModelAssetInstanceCardinality::kOnePerCell)
      .value("kMultiplePerCell", ModelAssetInstanceCardinality::kMultiplePerCell);

  py::class_<ModelSourceArtifactDTO>(module, "ModelSourceArtifactDTO", py::is_final())
      .def(py::init([](std::string logical_uri, std::string media_type, std::uint64_t byte_size,
                       ContentDigest content_digest) {
             return ModelSourceArtifactDTO{std::move(logical_uri), std::move(media_type), byte_size,
                                           std::move(content_digest)};
           }),
           py::arg("logical_uri"), py::arg("media_type"), py::arg("byte_size"),
           py::arg("content_digest"))
      .def_readonly("logical_uri", &ModelSourceArtifactDTO::logical_uri)
      .def_readonly("media_type", &ModelSourceArtifactDTO::media_type)
      .def_readonly("byte_size", &ModelSourceArtifactDTO::byte_size)
      .def_readonly("content_digest", &ModelSourceArtifactDTO::content_digest)
      .def(
          "__eq__",
          [](const ModelSourceArtifactDTO& lhs, const ModelSourceArtifactDTO& rhs) {
            return lhs == rhs;
          },
          py::is_operator())
      .def(
          "__ne__",
          [](const ModelSourceArtifactDTO& lhs, const ModelSourceArtifactDTO& rhs) {
            return lhs != rhs;
          },
          py::is_operator());

  py::class_<ExpandedModelSourceDTO>(module, "ExpandedModelSourceDTO", py::is_final())
      .def(py::init([](std::string urdf_logical_uri, std::vector<ModelSourceArtifactDTO> artifacts,
                       std::optional<std::string> srdf_logical_uri) {
             return ExpandedModelSourceDTO{std::move(urdf_logical_uri), std::move(srdf_logical_uri),
                                           std::move(artifacts)};
           }),
           py::arg("urdf_logical_uri"), py::arg("artifacts"),
           py::arg("srdf_logical_uri") = std::nullopt)
      .def_readonly("urdf_logical_uri", &ExpandedModelSourceDTO::urdf_logical_uri)
      .def_readonly("srdf_logical_uri", &ExpandedModelSourceDTO::srdf_logical_uri)
      .def_readonly("artifacts", &ExpandedModelSourceDTO::artifacts)
      .def(
          "__eq__",
          [](const ExpandedModelSourceDTO& lhs, const ExpandedModelSourceDTO& rhs) {
            return lhs == rhs;
          },
          py::is_operator())
      .def(
          "__ne__",
          [](const ExpandedModelSourceDTO& lhs, const ExpandedModelSourceDTO& rhs) {
            return lhs != rhs;
          },
          py::is_operator());

  py::class_<ModelFrameRoleBindingDTO>(module, "ModelFrameRoleBindingDTO", py::is_final())
      .def(py::init([](FrameRoleId frame_role_id, std::string source_link_name) {
             return ModelFrameRoleBindingDTO{std::move(frame_role_id), std::move(source_link_name)};
           }),
           py::arg("frame_role_id"), py::arg("source_link_name"))
      .def_readonly("frame_role_id", &ModelFrameRoleBindingDTO::frame_role_id)
      .def_readonly("source_link_name", &ModelFrameRoleBindingDTO::source_link_name)
      .def(
          "__eq__",
          [](const ModelFrameRoleBindingDTO& lhs, const ModelFrameRoleBindingDTO& rhs) {
            return lhs == rhs;
          },
          py::is_operator())
      .def(
          "__ne__",
          [](const ModelFrameRoleBindingDTO& lhs, const ModelFrameRoleBindingDTO& rhs) {
            return lhs != rhs;
          },
          py::is_operator());

  py::class_<ModelGroupRoleBindingDTO>(module, "ModelGroupRoleBindingDTO", py::is_final())
      .def(py::init([](ModelGroupRoleId model_group_role_id, std::string source_group_name) {
             return ModelGroupRoleBindingDTO{std::move(model_group_role_id),
                                             std::move(source_group_name)};
           }),
           py::arg("model_group_role_id"), py::arg("source_group_name"))
      .def_readonly("model_group_role_id", &ModelGroupRoleBindingDTO::model_group_role_id)
      .def_readonly("source_group_name", &ModelGroupRoleBindingDTO::source_group_name)
      .def(
          "__eq__",
          [](const ModelGroupRoleBindingDTO& lhs, const ModelGroupRoleBindingDTO& rhs) {
            return lhs == rhs;
          },
          py::is_operator())
      .def(
          "__ne__",
          [](const ModelGroupRoleBindingDTO& lhs, const ModelGroupRoleBindingDTO& rhs) {
            return lhs != rhs;
          },
          py::is_operator());

  py::class_<ModelResourceRoleBindingDTO>(module, "ModelResourceRoleBindingDTO", py::is_final())
      .def(py::init(
               [](ResourceRoleId resource_role_id, std::vector<std::string> source_joint_names) {
                 return ModelResourceRoleBindingDTO{std::move(resource_role_id),
                                                    std::move(source_joint_names)};
               }),
           py::arg("resource_role_id"), py::arg("source_joint_names"))
      .def_readonly("resource_role_id", &ModelResourceRoleBindingDTO::resource_role_id)
      .def_readonly("source_joint_names", &ModelResourceRoleBindingDTO::source_joint_names)
      .def(
          "__eq__",
          [](const ModelResourceRoleBindingDTO& lhs, const ModelResourceRoleBindingDTO& rhs) {
            return lhs == rhs;
          },
          py::is_operator())
      .def(
          "__ne__",
          [](const ModelResourceRoleBindingDTO& lhs, const ModelResourceRoleBindingDTO& rhs) {
            return lhs != rhs;
          },
          py::is_operator());

  py::class_<ModelCollisionBodyRoleBindingDTO>(module, "ModelCollisionBodyRoleBindingDTO",
                                               py::is_final())
      .def(py::init([](CollisionBodyRoleId collision_body_role_id,
                       std::vector<std::string> source_link_names) {
             return ModelCollisionBodyRoleBindingDTO{std::move(collision_body_role_id),
                                                     std::move(source_link_names)};
           }),
           py::arg("collision_body_role_id"), py::arg("source_link_names"))
      .def_readonly("collision_body_role_id",
                    &ModelCollisionBodyRoleBindingDTO::collision_body_role_id)
      .def_readonly("source_link_names", &ModelCollisionBodyRoleBindingDTO::source_link_names)
      .def(
          "__eq__",
          [](const ModelCollisionBodyRoleBindingDTO& lhs,
             const ModelCollisionBodyRoleBindingDTO& rhs) { return lhs == rhs; },
          py::is_operator())
      .def(
          "__ne__",
          [](const ModelCollisionBodyRoleBindingDTO& lhs,
             const ModelCollisionBodyRoleBindingDTO& rhs) { return lhs != rhs; },
          py::is_operator());

  py::class_<ModelAssetDTO>(module, "ModelAssetDTO", py::is_final())
      .def(py::init([](ModelAssetId model_asset_id, ModelRevision model_revision,
                       ModelAssetInstanceCardinality instance_cardinality,
                       ExpandedModelSourceDTO expanded_source, FrameRoleId root_frame_role_id,
                       std::vector<ModelFrameRoleBindingDTO> frame_role_bindings,
                       std::vector<ModelGroupRoleBindingDTO> model_group_role_bindings,
                       std::vector<ModelResourceRoleBindingDTO> resource_role_bindings,
                       std::vector<ModelCollisionBodyRoleBindingDTO> collision_body_role_bindings) {
             return ModelAssetDTO{std::move(model_asset_id),
                                  std::move(model_revision),
                                  instance_cardinality,
                                  std::move(expanded_source),
                                  std::move(root_frame_role_id),
                                  std::move(frame_role_bindings),
                                  std::move(model_group_role_bindings),
                                  std::move(resource_role_bindings),
                                  std::move(collision_body_role_bindings)};
           }),
           py::arg("model_asset_id"), py::arg("model_revision"), py::arg("instance_cardinality"),
           py::arg("expanded_source"), py::arg("root_frame_role_id"),
           py::arg("frame_role_bindings"), py::arg("model_group_role_bindings"),
           py::arg("resource_role_bindings"), py::arg("collision_body_role_bindings"))
      .def_readonly("model_asset_id", &ModelAssetDTO::model_asset_id)
      .def_readonly("model_revision", &ModelAssetDTO::model_revision)
      .def_readonly("instance_cardinality", &ModelAssetDTO::instance_cardinality)
      .def_readonly("expanded_source", &ModelAssetDTO::expanded_source)
      .def_readonly("root_frame_role_id", &ModelAssetDTO::root_frame_role_id)
      .def_readonly("frame_role_bindings", &ModelAssetDTO::frame_role_bindings)
      .def_readonly("model_group_role_bindings", &ModelAssetDTO::model_group_role_bindings)
      .def_readonly("resource_role_bindings", &ModelAssetDTO::resource_role_bindings)
      .def_readonly("collision_body_role_bindings", &ModelAssetDTO::collision_body_role_bindings)
      .def(
          "__eq__", [](const ModelAssetDTO& lhs, const ModelAssetDTO& rhs) { return lhs == rhs; },
          py::is_operator())
      .def(
          "__ne__", [](const ModelAssetDTO& lhs, const ModelAssetDTO& rhs) { return lhs != rhs; },
          py::is_operator());

  module.attr("MODEL_ASSET_API_VERSION") = std::string(kModelAssetApiVersion);
  module.attr("MAX_MODEL_ASSET_COLLECTION_ITEMS") = kMaxModelAssetCollectionItems;
  module.attr("MAX_MODEL_ASSET_LOGICAL_URI_BYTES") = kMaxModelAssetLogicalUriBytes;
  module.attr("MAX_MODEL_ASSET_MEDIA_TYPE_BYTES") = kMaxModelAssetMediaTypeBytes;
  module.attr("MAX_MODEL_ASSET_SOURCE_SYMBOL_CODE_POINTS") = kMaxModelAssetSourceSymbolCodePoints;
  module.attr("MAX_MODEL_ASSET_BYTE_SIZE") = kMaxModelAssetByteSize;

  py::enum_<CapabilityScope>(module, "CapabilityScope")
      .value("kCellGlobal", CapabilityScope::kCellGlobal)
      .value("kControlDomain", CapabilityScope::kControlDomain)
      .value("kTargetBinding", CapabilityScope::kTargetBinding);

  py::enum_<CapabilityProviderBindingKind>(module, "CapabilityProviderBindingKind")
      .value("kCapabilityProviderBinding",
             CapabilityProviderBindingKind::kCapabilityProviderBinding)
      .value("kControlBinding", CapabilityProviderBindingKind::kControlBinding);

  py::class_<ValueSchemaRefDTO>(module, "ValueSchemaRefDTO", py::is_final())
      .def(py::init([](SchemaId schema_id, SchemaRevision schema_revision) {
             return ValueSchemaRefDTO{std::move(schema_id), std::move(schema_revision)};
           }),
           py::arg("schema_id"), py::arg("schema_revision"))
      .def_readonly("schema_id", &ValueSchemaRefDTO::schema_id)
      .def_readonly("schema_revision", &ValueSchemaRefDTO::schema_revision)
      .def(
          "__eq__",
          [](const ValueSchemaRefDTO& lhs, const ValueSchemaRefDTO& rhs) { return lhs == rhs; },
          py::is_operator())
      .def(
          "__ne__",
          [](const ValueSchemaRefDTO& lhs, const ValueSchemaRefDTO& rhs) { return lhs != rhs; },
          py::is_operator());

  py::enum_<RuntimeLifecycleControl>(module, "RuntimeLifecycleControl")
      .value("kManagerManaged", RuntimeLifecycleControl::kManagerManaged)
      .value("kExternallyManaged", RuntimeLifecycleControl::kExternallyManaged);

  py::class_<RuntimeProcessContractDTO>(module, "RuntimeProcessContractDTO", py::is_final())
      .def(py::init([](RuntimeLifecycleControl lifecycle_control) {
             return RuntimeProcessContractDTO{lifecycle_control};
           }),
           py::arg("lifecycle_control"))
      .def_readonly("lifecycle_control", &RuntimeProcessContractDTO::lifecycle_control)
      .def(
          "__eq__",
          [](const RuntimeProcessContractDTO& lhs, const RuntimeProcessContractDTO& rhs) {
            return lhs == rhs;
          },
          py::is_operator())
      .def(
          "__ne__",
          [](const RuntimeProcessContractDTO& lhs, const RuntimeProcessContractDTO& rhs) {
            return lhs != rhs;
          },
          py::is_operator());

  py::class_<RuntimeSupervisionContractDTO>(module, "RuntimeSupervisionContractDTO", py::is_final())
      .def(py::init([](EndpointRoleId status_endpoint_role_id) {
             return RuntimeSupervisionContractDTO{std::move(status_endpoint_role_id)};
           }),
           py::arg("status_endpoint_role_id"))
      .def_readonly("status_endpoint_role_id",
                    &RuntimeSupervisionContractDTO::status_endpoint_role_id)
      .def(
          "__eq__",
          [](const RuntimeSupervisionContractDTO& lhs, const RuntimeSupervisionContractDTO& rhs) {
            return lhs == rhs;
          },
          py::is_operator())
      .def(
          "__ne__",
          [](const RuntimeSupervisionContractDTO& lhs, const RuntimeSupervisionContractDTO& rhs) {
            return lhs != rhs;
          },
          py::is_operator());

  py::class_<RuntimeProfileDTO>(module, "RuntimeProfileDTO", py::is_final())
      .def(py::init([](RuntimeProfileId runtime_profile_id,
                       RuntimeProfileRevision runtime_profile_revision,
                       ValueSchemaRefDTO configuration_schema_ref,
                       RuntimeProcessContractDTO process_contract,
                       RuntimeSupervisionContractDTO supervision_contract,
                       std::vector<RuntimeRoleId> offered_runtime_role_ids,
                       std::vector<EndpointRoleId> source_endpoint_role_ids) {
             return RuntimeProfileDTO{
                 std::move(runtime_profile_id),       std::move(runtime_profile_revision),
                 std::move(configuration_schema_ref), std::move(process_contract),
                 std::move(supervision_contract),     std::move(offered_runtime_role_ids),
                 std::move(source_endpoint_role_ids)};
           }),
           py::arg("runtime_profile_id"), py::arg("runtime_profile_revision"),
           py::arg("configuration_schema_ref"), py::arg("process_contract"),
           py::arg("supervision_contract"), py::arg("offered_runtime_role_ids"),
           py::arg("source_endpoint_role_ids"))
      .def_readonly("runtime_profile_id", &RuntimeProfileDTO::runtime_profile_id)
      .def_readonly("runtime_profile_revision", &RuntimeProfileDTO::runtime_profile_revision)
      .def_readonly("configuration_schema_ref", &RuntimeProfileDTO::configuration_schema_ref)
      .def_readonly("process_contract", &RuntimeProfileDTO::process_contract)
      .def_readonly("supervision_contract", &RuntimeProfileDTO::supervision_contract)
      .def_readonly("offered_runtime_role_ids", &RuntimeProfileDTO::offered_runtime_role_ids)
      .def_readonly("source_endpoint_role_ids", &RuntimeProfileDTO::source_endpoint_role_ids)
      .def(
          "__eq__",
          [](const RuntimeProfileDTO& lhs, const RuntimeProfileDTO& rhs) { return lhs == rhs; },
          py::is_operator())
      .def(
          "__ne__",
          [](const RuntimeProfileDTO& lhs, const RuntimeProfileDTO& rhs) { return lhs != rhs; },
          py::is_operator());

  module.attr("RUNTIME_PROFILE_API_VERSION") = std::string(kRuntimeProfileApiVersion);
  module.attr("MAX_RUNTIME_PROFILE_ROLE_IDS") = kMaxRuntimeProfileRoleIds;

  py::class_<CapabilityDefinitionDTO>(module, "CapabilityDefinitionDTO", py::is_final())
      .def(py::init([](CapabilityId capability_id, CapabilityDefinitionRevision revision,
                       CapabilityScope scope, CapabilityProviderBindingKind provider_binding_kind,
                       std::optional<ValueSchemaRefDTO> schema_ref) {
             return CapabilityDefinitionDTO{std::move(capability_id), std::move(revision), scope,
                                            provider_binding_kind, std::move(schema_ref)};
           }),
           py::arg("capability_id"), py::arg("capability_definition_revision"), py::arg("scope"),
           py::arg("provider_binding_kind"),
           py::arg("requirement_parameters_schema_ref") = std::nullopt)
      .def_readonly("capability_id", &CapabilityDefinitionDTO::capability_id)
      .def_readonly("capability_definition_revision",
                    &CapabilityDefinitionDTO::capability_definition_revision)
      .def_readonly("scope", &CapabilityDefinitionDTO::scope)
      .def_readonly("provider_binding_kind", &CapabilityDefinitionDTO::provider_binding_kind)
      .def_readonly("requirement_parameters_schema_ref",
                    &CapabilityDefinitionDTO::requirement_parameters_schema_ref)
      .def(
          "__eq__",
          [](const CapabilityDefinitionDTO& lhs, const CapabilityDefinitionDTO& rhs) {
            return lhs == rhs;
          },
          py::is_operator())
      .def(
          "__ne__",
          [](const CapabilityDefinitionDTO& lhs, const CapabilityDefinitionDTO& rhs) {
            return lhs != rhs;
          },
          py::is_operator());

  py::class_<ResourceInterfaceClaimDTO>(module, "ResourceInterfaceClaimDTO", py::is_final())
      .def(py::init([](ResourceRoleId resource_role_id, std::string interface_name) {
             return ResourceInterfaceClaimDTO{std::move(resource_role_id),
                                              std::move(interface_name)};
           }),
           py::arg("resource_role_id"), py::arg("interface_name"))
      .def_readonly("resource_role_id", &ResourceInterfaceClaimDTO::resource_role_id)
      .def_readonly("interface_name", &ResourceInterfaceClaimDTO::interface_name)
      .def(
          "__eq__",
          [](const ResourceInterfaceClaimDTO& lhs, const ResourceInterfaceClaimDTO& rhs) {
            return lhs == rhs;
          },
          py::is_operator())
      .def(
          "__ne__",
          [](const ResourceInterfaceClaimDTO& lhs, const ResourceInterfaceClaimDTO& rhs) {
            return lhs != rhs;
          },
          py::is_operator());

  py::class_<ControlInterfaceContractDTO>(module, "ControlInterfaceContractDTO", py::is_final())
      .def(py::init([](ControlInterfaceContractId control_interface_contract_id,
                       ControlInterfaceRevision control_interface_revision,
                       std::vector<ResourceInterfaceClaimDTO> command_interfaces,
                       std::vector<ResourceInterfaceClaimDTO> state_interfaces,
                       std::vector<ResourceRoleId> ordering) {
             return ControlInterfaceContractDTO{
                 std::move(control_interface_contract_id), std::move(control_interface_revision),
                 std::move(command_interfaces), std::move(state_interfaces), std::move(ordering)};
           }),
           py::arg("control_interface_contract_id"), py::arg("control_interface_revision"),
           py::arg("command_interfaces"), py::arg("state_interfaces"), py::arg("ordering"))
      .def_readonly("control_interface_contract_id",
                    &ControlInterfaceContractDTO::control_interface_contract_id)
      .def_readonly("control_interface_revision",
                    &ControlInterfaceContractDTO::control_interface_revision)
      .def_readonly("command_interfaces", &ControlInterfaceContractDTO::command_interfaces)
      .def_readonly("state_interfaces", &ControlInterfaceContractDTO::state_interfaces)
      .def_readonly("ordering", &ControlInterfaceContractDTO::ordering)
      .def(
          "__eq__",
          [](const ControlInterfaceContractDTO& lhs, const ControlInterfaceContractDTO& rhs) {
            return lhs == rhs;
          },
          py::is_operator())
      .def(
          "__ne__",
          [](const ControlInterfaceContractDTO& lhs, const ControlInterfaceContractDTO& rhs) {
            return lhs != rhs;
          },
          py::is_operator());

  module.attr("CONTROL_INTERFACE_CONTRACT_API_VERSION") =
      std::string(kControlInterfaceContractApiVersion);
  module.attr("MAX_CONTROL_INTERFACE_CONTRACT_ITEMS") = kMaxControlInterfaceContractItems;

  py::enum_<ControllerClaimDirection>(module, "ControllerClaimDirection")
      .value("kCommand", ControllerClaimDirection::kCommand)
      .value("kState", ControllerClaimDirection::kState);

  py::class_<ControllerControlInterfaceClaimDTO>(module, "ControllerControlInterfaceClaimDTO",
                                                 py::is_final())
      .def(py::init([](ControlInterfaceContractId control_interface_contract_id,
                       ControlInterfaceRevision control_interface_revision,
                       std::vector<ControllerClaimDirection> claimed_directions) {
             return ControllerControlInterfaceClaimDTO{std::move(control_interface_contract_id),
                                                       std::move(control_interface_revision),
                                                       std::move(claimed_directions)};
           }),
           py::arg("control_interface_contract_id"), py::arg("control_interface_revision"),
           py::arg("claimed_directions"))
      .def_readonly("control_interface_contract_id",
                    &ControllerControlInterfaceClaimDTO::control_interface_contract_id)
      .def_readonly("control_interface_revision",
                    &ControllerControlInterfaceClaimDTO::control_interface_revision)
      .def_readonly("claimed_directions", &ControllerControlInterfaceClaimDTO::claimed_directions)
      .def(
          "__eq__",
          [](const ControllerControlInterfaceClaimDTO& lhs,
             const ControllerControlInterfaceClaimDTO& rhs) { return lhs == rhs; },
          py::is_operator())
      .def(
          "__ne__",
          [](const ControllerControlInterfaceClaimDTO& lhs,
             const ControllerControlInterfaceClaimDTO& rhs) { return lhs != rhs; },
          py::is_operator());

  py::class_<ControllerTopologyControllerDTO>(module, "ControllerTopologyControllerDTO",
                                              py::is_final())
      .def(py::init([](ControllerRoleId controller_role_id,
                       std::vector<ControllerControlInterfaceClaimDTO> control_interface_claims,
                       std::vector<ResourceInterfaceClaimDTO> exported_reference_interfaces,
                       std::vector<ResourceInterfaceClaimDTO> exported_state_interfaces) {
             return ControllerTopologyControllerDTO{
                 std::move(controller_role_id), std::move(control_interface_claims),
                 std::move(exported_reference_interfaces), std::move(exported_state_interfaces)};
           }),
           py::arg("controller_role_id"), py::arg("control_interface_claims"),
           py::arg("exported_reference_interfaces"), py::arg("exported_state_interfaces"))
      .def_readonly("controller_role_id", &ControllerTopologyControllerDTO::controller_role_id)
      .def_readonly("control_interface_claims",
                    &ControllerTopologyControllerDTO::control_interface_claims)
      .def_readonly("exported_reference_interfaces",
                    &ControllerTopologyControllerDTO::exported_reference_interfaces)
      .def_readonly("exported_state_interfaces",
                    &ControllerTopologyControllerDTO::exported_state_interfaces)
      .def(
          "__eq__",
          [](const ControllerTopologyControllerDTO& lhs,
             const ControllerTopologyControllerDTO& rhs) { return lhs == rhs; },
          py::is_operator())
      .def(
          "__ne__",
          [](const ControllerTopologyControllerDTO& lhs,
             const ControllerTopologyControllerDTO& rhs) { return lhs != rhs; },
          py::is_operator());

  py::class_<ControllerChainEdgeDTO>(module, "ControllerChainEdgeDTO", py::is_final())
      .def(py::init([](ControllerRoleId upstream_controller_role_id,
                       ControllerRoleId downstream_controller_role_id,
                       std::vector<ResourceInterfaceClaimDTO> reference_interfaces,
                       std::vector<ResourceInterfaceClaimDTO> state_interfaces) {
             return ControllerChainEdgeDTO{
                 std::move(upstream_controller_role_id), std::move(downstream_controller_role_id),
                 std::move(reference_interfaces), std::move(state_interfaces)};
           }),
           py::arg("upstream_controller_role_id"), py::arg("downstream_controller_role_id"),
           py::arg("reference_interfaces"), py::arg("state_interfaces"))
      .def_readonly("upstream_controller_role_id",
                    &ControllerChainEdgeDTO::upstream_controller_role_id)
      .def_readonly("downstream_controller_role_id",
                    &ControllerChainEdgeDTO::downstream_controller_role_id)
      .def_readonly("reference_interfaces", &ControllerChainEdgeDTO::reference_interfaces)
      .def_readonly("state_interfaces", &ControllerChainEdgeDTO::state_interfaces)
      .def(
          "__eq__",
          [](const ControllerChainEdgeDTO& lhs, const ControllerChainEdgeDTO& rhs) {
            return lhs == rhs;
          },
          py::is_operator())
      .def(
          "__ne__",
          [](const ControllerChainEdgeDTO& lhs, const ControllerChainEdgeDTO& rhs) {
            return lhs != rhs;
          },
          py::is_operator());

  py::class_<ControllerChainEdgeRefDTO>(module, "ControllerChainEdgeRefDTO", py::is_final())
      .def(py::init([](ControllerRoleId upstream_controller_role_id,
                       ControllerRoleId downstream_controller_role_id) {
             return ControllerChainEdgeRefDTO{std::move(upstream_controller_role_id),
                                              std::move(downstream_controller_role_id)};
           }),
           py::arg("upstream_controller_role_id"), py::arg("downstream_controller_role_id"))
      .def_readonly("upstream_controller_role_id",
                    &ControllerChainEdgeRefDTO::upstream_controller_role_id)
      .def_readonly("downstream_controller_role_id",
                    &ControllerChainEdgeRefDTO::downstream_controller_role_id)
      .def(
          "__eq__",
          [](const ControllerChainEdgeRefDTO& lhs, const ControllerChainEdgeRefDTO& rhs) {
            return lhs == rhs;
          },
          py::is_operator())
      .def(
          "__ne__",
          [](const ControllerChainEdgeRefDTO& lhs, const ControllerChainEdgeRefDTO& rhs) {
            return lhs != rhs;
          },
          py::is_operator());

  py::class_<ControllerActivationSetDTO>(module, "ControllerActivationSetDTO", py::is_final())
      .def(py::init([](ActivationSetId activation_set_id,
                       std::vector<ControllerRoleId> active_controller_role_ids,
                       std::vector<ControllerChainEdgeRefDTO> active_chain_edges) {
             return ControllerActivationSetDTO{std::move(activation_set_id),
                                               std::move(active_controller_role_ids),
                                               std::move(active_chain_edges)};
           }),
           py::arg("activation_set_id"), py::arg("active_controller_role_ids"),
           py::arg("active_chain_edges"))
      .def_readonly("activation_set_id", &ControllerActivationSetDTO::activation_set_id)
      .def_readonly("active_controller_role_ids",
                    &ControllerActivationSetDTO::active_controller_role_ids)
      .def_readonly("active_chain_edges", &ControllerActivationSetDTO::active_chain_edges)
      .def(
          "__eq__",
          [](const ControllerActivationSetDTO& lhs, const ControllerActivationSetDTO& rhs) {
            return lhs == rhs;
          },
          py::is_operator())
      .def(
          "__ne__",
          [](const ControllerActivationSetDTO& lhs, const ControllerActivationSetDTO& rhs) {
            return lhs != rhs;
          },
          py::is_operator());

  py::class_<ControlRouteDTO>(module, "ControlRouteDTO", py::is_final())
      .def(py::init([](ControlRouteId control_route_id,
                       ControlInterfaceContractId control_interface_contract_id,
                       ControlInterfaceRevision control_interface_revision,
                       std::vector<ControllerRoleId> ordered_controller_role_ids,
                       std::vector<ActivationSetId> compatible_activation_set_ids) {
             return ControlRouteDTO{
                 std::move(control_route_id), std::move(control_interface_contract_id),
                 std::move(control_interface_revision), std::move(ordered_controller_role_ids),
                 std::move(compatible_activation_set_ids)};
           }),
           py::arg("control_route_id"), py::arg("control_interface_contract_id"),
           py::arg("control_interface_revision"), py::arg("ordered_controller_role_ids"),
           py::arg("compatible_activation_set_ids"))
      .def_readonly("control_route_id", &ControlRouteDTO::control_route_id)
      .def_readonly("control_interface_contract_id",
                    &ControlRouteDTO::control_interface_contract_id)
      .def_readonly("control_interface_revision", &ControlRouteDTO::control_interface_revision)
      .def_readonly("ordered_controller_role_ids", &ControlRouteDTO::ordered_controller_role_ids)
      .def_readonly("compatible_activation_set_ids",
                    &ControlRouteDTO::compatible_activation_set_ids)
      .def(
          "__eq__",
          [](const ControlRouteDTO& lhs, const ControlRouteDTO& rhs) { return lhs == rhs; },
          py::is_operator())
      .def(
          "__ne__",
          [](const ControlRouteDTO& lhs, const ControlRouteDTO& rhs) { return lhs != rhs; },
          py::is_operator());

  py::class_<ControllerTopologyDTO>(module, "ControllerTopologyDTO", py::is_final())
      .def(py::init([](ControllerTopologyId controller_topology_id,
                       TopologyRevision topology_revision,
                       std::vector<ControllerTopologyControllerDTO> controllers,
                       std::vector<ControllerChainEdgeDTO> chain_edges,
                       std::vector<ControllerActivationSetDTO> activation_sets,
                       std::vector<ControlRouteDTO> legal_routes) {
             return ControllerTopologyDTO{std::move(controller_topology_id),
                                          std::move(topology_revision),
                                          std::move(controllers),
                                          std::move(chain_edges),
                                          std::move(activation_sets),
                                          std::move(legal_routes)};
           }),
           py::arg("controller_topology_id"), py::arg("topology_revision"), py::arg("controllers"),
           py::arg("chain_edges"), py::arg("activation_sets"), py::arg("legal_routes"))
      .def_readonly("controller_topology_id", &ControllerTopologyDTO::controller_topology_id)
      .def_readonly("topology_revision", &ControllerTopologyDTO::topology_revision)
      .def_readonly("controllers", &ControllerTopologyDTO::controllers)
      .def_readonly("chain_edges", &ControllerTopologyDTO::chain_edges)
      .def_readonly("activation_sets", &ControllerTopologyDTO::activation_sets)
      .def_readonly("legal_routes", &ControllerTopologyDTO::legal_routes)
      .def(
          "__eq__",
          [](const ControllerTopologyDTO& lhs, const ControllerTopologyDTO& rhs) {
            return lhs == rhs;
          },
          py::is_operator())
      .def(
          "__ne__",
          [](const ControllerTopologyDTO& lhs, const ControllerTopologyDTO& rhs) {
            return lhs != rhs;
          },
          py::is_operator());

  module.attr("CONTROLLER_TOPOLOGY_API_VERSION") = std::string(kControllerTopologyApiVersion);
  module.attr("MAX_CONTROLLER_TOPOLOGY_CONTROLLERS") = kMaxControllerTopologyControllers;
  module.attr("MAX_CONTROLLER_TOPOLOGY_CONTROL_INTERFACE_CLAIMS_PER_CONTROLLER") =
      kMaxControllerTopologyControlInterfaceClaimsPerController;
  module.attr("MAX_CONTROLLER_TOPOLOGY_CLAIMED_DIRECTIONS_PER_CLAIM") =
      kMaxControllerTopologyClaimedDirectionsPerClaim;
  module.attr("MAX_CONTROLLER_TOPOLOGY_EXPORTED_REFERENCE_INTERFACES_PER_CONTROLLER") =
      kMaxControllerTopologyExportedReferenceInterfacesPerController;
  module.attr("MAX_CONTROLLER_TOPOLOGY_EXPORTED_STATE_INTERFACES_PER_CONTROLLER") =
      kMaxControllerTopologyExportedStateInterfacesPerController;
  module.attr("MAX_CONTROLLER_TOPOLOGY_CHAIN_EDGES") = kMaxControllerTopologyChainEdges;
  module.attr("MAX_CONTROLLER_TOPOLOGY_REFERENCE_INTERFACES_PER_CHAIN_EDGE") =
      kMaxControllerTopologyReferenceInterfacesPerChainEdge;
  module.attr("MAX_CONTROLLER_TOPOLOGY_STATE_INTERFACES_PER_CHAIN_EDGE") =
      kMaxControllerTopologyStateInterfacesPerChainEdge;
  module.attr("MAX_CONTROLLER_TOPOLOGY_ACTIVATION_SETS") = kMaxControllerTopologyActivationSets;
  module.attr("MAX_CONTROLLER_TOPOLOGY_ACTIVE_CONTROLLER_ROLES_PER_ACTIVATION_SET") =
      kMaxControllerTopologyActiveControllerRolesPerActivationSet;
  module.attr("MAX_CONTROLLER_TOPOLOGY_ACTIVE_CHAIN_EDGES_PER_ACTIVATION_SET") =
      kMaxControllerTopologyActiveChainEdgesPerActivationSet;
  module.attr("MAX_CONTROLLER_TOPOLOGY_CONTROL_ROUTES") = kMaxControllerTopologyControlRoutes;
  module.attr("MAX_CONTROLLER_TOPOLOGY_ORDERED_CONTROLLER_ROLES_PER_ROUTE") =
      kMaxControllerTopologyOrderedControllerRolesPerRoute;
  module.attr("MAX_CONTROLLER_TOPOLOGY_COMPATIBLE_ACTIVATION_SETS_PER_ROUTE") =
      kMaxControllerTopologyCompatibleActivationSetsPerRoute;
  module.attr("MAX_CONTROLLER_TOPOLOGY_DISTINCT_CONTROL_INTERFACE_CONTRACTS") =
      kMaxControllerTopologyDistinctControlInterfaceContracts;

  py::enum_<StopEvidenceDisposition>(module, "StopEvidenceDisposition")
      .value("kStoppable", StopEvidenceDisposition::kStoppable)
      .value("kNotStoppable", StopEvidenceDisposition::kNotStoppable);
  py::enum_<StopContainmentCommandKind>(module, "StopContainmentCommandKind")
      .value("kControllerGoalCancel", StopContainmentCommandKind::kControllerGoalCancel)
      .value("kMotionControlledStop", StopContainmentCommandKind::kMotionControlledStop)
      .value("kCapabilityCommandCancel", StopContainmentCommandKind::kCapabilityCommandCancel)
      .value("kSafeOutputCommand", StopContainmentCommandKind::kSafeOutputCommand)
      .value("kLogicalCancel", StopContainmentCommandKind::kLogicalCancel)
      .value("kRegistered", StopContainmentCommandKind::kRegistered);
  py::enum_<StopEscalationStageKind>(module, "StopEscalationStageKind")
      .value("kNominal", StopEscalationStageKind::kNominal)
      .value("kControlledFallback", StopEscalationStageKind::kControlledFallback)
      .value("kSoftwareHard", StopEscalationStageKind::kSoftwareHard)
      .value("kSafetySystemRequest", StopEscalationStageKind::kSafetySystemRequest);
  py::enum_<StopActionClass>(module, "StopActionClass")
      .value("kControlled", StopActionClass::kControlled)
      .value("kSoftwareHard", StopActionClass::kSoftwareHard)
      .value("kSafetySystemRequest", StopActionClass::kSafetySystemRequest);
  py::enum_<SafetySystemRequestActionClass>(module, "SafetySystemRequestActionClass")
      .value("kProtectiveStopRequest", SafetySystemRequestActionClass::kProtectiveStopRequest)
      .value("kEmergencyStopRequest", SafetySystemRequestActionClass::kEmergencyStopRequest);
  py::enum_<StopEvidenceConjunction>(module, "StopEvidenceConjunction")
      .value("kAllOf", StopEvidenceConjunction::kAllOf);
  py::enum_<StopEvidenceRequirementKind>(module, "StopEvidenceRequirementKind")
      .value("kControllerGoalTerminal", StopEvidenceRequirementKind::kControllerGoalTerminal)
      .value("kMotionRest", StopEvidenceRequirementKind::kMotionRest)
      .value("kDiscreteAcknowledgement", StopEvidenceRequirementKind::kDiscreteAcknowledgement)
      .value("kIoSafeOutput", StopEvidenceRequirementKind::kIoSafeOutput)
      .value("kLogicalCancellationNoContinuingEffect",
             StopEvidenceRequirementKind::kLogicalCancellationNoContinuingEffect)
      .value("kRegistered", StopEvidenceRequirementKind::kRegistered);
  py::enum_<StopEvidenceSourceKind>(module, "StopEvidenceSourceKind")
      .value("kControllerGoalStatus", StopEvidenceSourceKind::kControllerGoalStatus)
      .value("kBindingStateAuthority", StopEvidenceSourceKind::kBindingStateAuthority)
      .value("kCommandTargetStatus", StopEvidenceSourceKind::kCommandTargetStatus)
      .value("kProviderEvidence", StopEvidenceSourceKind::kProviderEvidence);
  py::enum_<StopEvidenceObservationKind>(module, "StopEvidenceObservationKind")
      .value("kCommandResult", StopEvidenceObservationKind::kCommandResult)
      .value("kPostCommandSample", StopEvidenceObservationKind::kPostCommandSample)
      .value("kPostCommandDwell", StopEvidenceObservationKind::kPostCommandDwell);

  py::class_<ProviderContractRefDTO>(module, "ProviderContractRefDTO", py::is_final())
      .def(py::init<ProviderContractRoleId, SchemaId, SchemaRevision>(),
           py::arg("provider_contract_role_id"), py::arg("contract_schema_id"),
           py::arg("contract_schema_revision"))
      .def_readonly("provider_contract_role_id", &ProviderContractRefDTO::provider_contract_role_id)
      .def_readonly("contract_schema_id", &ProviderContractRefDTO::contract_schema_id)
      .def_readonly("contract_schema_revision", &ProviderContractRefDTO::contract_schema_revision)
      .def(
          "__eq__",
          [](const ProviderContractRefDTO& lhs, const ProviderContractRefDTO& rhs) {
            return lhs == rhs;
          },
          py::is_operator());

  py::class_<ProviderCapabilityOfferDTO>(module, "ProviderCapabilityOfferDTO", py::is_final())
      .def(py::init([](CapabilityId capability_id,
                       CapabilityDefinitionRevision capability_definition_revision,
                       std::vector<EndpointRoleId> required_endpoint_role_ids) {
             return ProviderCapabilityOfferDTO{std::move(capability_id),
                                               std::move(capability_definition_revision),
                                               std::move(required_endpoint_role_ids)};
           }),
           py::arg("capability_id"), py::arg("capability_definition_revision"),
           py::arg("required_endpoint_role_ids"))
      .def_readonly("capability_id", &ProviderCapabilityOfferDTO::capability_id)
      .def_readonly("capability_definition_revision",
                    &ProviderCapabilityOfferDTO::capability_definition_revision)
      .def_readonly("required_endpoint_role_ids",
                    &ProviderCapabilityOfferDTO::required_endpoint_role_ids)
      .def(
          "__eq__",
          [](const ProviderCapabilityOfferDTO& lhs, const ProviderCapabilityOfferDTO& rhs) {
            return lhs == rhs;
          },
          py::is_operator())
      .def(
          "__ne__",
          [](const ProviderCapabilityOfferDTO& lhs, const ProviderCapabilityOfferDTO& rhs) {
            return lhs != rhs;
          },
          py::is_operator());

  py::class_<ProviderStopCommandSupportDTO>(module, "ProviderStopCommandSupportDTO", py::is_final())
      .def(py::init([](StopContainmentCommandKind command_kind,
                       std::vector<EndpointRoleId> command_endpoint_role_ids,
                       std::optional<RegisteredStopCommandKindId> registered_command_kind_id) {
             return ProviderStopCommandSupportDTO{command_kind,
                                                  std::move(registered_command_kind_id),
                                                  std::move(command_endpoint_role_ids)};
           }),
           py::arg("command_kind"), py::arg("command_endpoint_role_ids"),
           py::arg("registered_command_kind_id") = std::nullopt)
      .def_readonly("command_kind", &ProviderStopCommandSupportDTO::command_kind)
      .def_readonly("registered_command_kind_id",
                    &ProviderStopCommandSupportDTO::registered_command_kind_id)
      .def_readonly("command_endpoint_role_ids",
                    &ProviderStopCommandSupportDTO::command_endpoint_role_ids)
      .def(
          "__eq__",
          [](const ProviderStopCommandSupportDTO& lhs, const ProviderStopCommandSupportDTO& rhs) {
            return lhs == rhs;
          },
          py::is_operator())
      .def(
          "__ne__",
          [](const ProviderStopCommandSupportDTO& lhs, const ProviderStopCommandSupportDTO& rhs) {
            return lhs != rhs;
          },
          py::is_operator());

  py::class_<ProviderStopEvidenceSupportDTO>(module, "ProviderStopEvidenceSupportDTO",
                                             py::is_final())
      .def(py::init([](StopEvidenceRequirementKind evidence_kind,
                       std::vector<EndpointRoleId> evidence_endpoint_role_ids,
                       std::optional<RegisteredStopEvidenceKindId> registered_evidence_kind_id) {
             return ProviderStopEvidenceSupportDTO{evidence_kind,
                                                   std::move(registered_evidence_kind_id),
                                                   std::move(evidence_endpoint_role_ids)};
           }),
           py::arg("evidence_kind"), py::arg("evidence_endpoint_role_ids"),
           py::arg("registered_evidence_kind_id") = std::nullopt)
      .def_readonly("evidence_kind", &ProviderStopEvidenceSupportDTO::evidence_kind)
      .def_readonly("registered_evidence_kind_id",
                    &ProviderStopEvidenceSupportDTO::registered_evidence_kind_id)
      .def_readonly("evidence_endpoint_role_ids",
                    &ProviderStopEvidenceSupportDTO::evidence_endpoint_role_ids)
      .def(
          "__eq__",
          [](const ProviderStopEvidenceSupportDTO& lhs, const ProviderStopEvidenceSupportDTO& rhs) {
            return lhs == rhs;
          },
          py::is_operator())
      .def(
          "__ne__",
          [](const ProviderStopEvidenceSupportDTO& lhs, const ProviderStopEvidenceSupportDTO& rhs) {
            return lhs != rhs;
          },
          py::is_operator());

  py::class_<ProviderContractDeclarationDTO>(module, "ProviderContractDeclarationDTO",
                                             py::is_final())
      .def(py::init([](ProviderContractRefDTO provider_contract_ref,
                       std::optional<ProviderStopCommandSupportDTO> stop_command_support,
                       std::optional<ProviderStopEvidenceSupportDTO> stop_evidence_support) {
             return ProviderContractDeclarationDTO{std::move(provider_contract_ref),
                                                   std::move(stop_command_support),
                                                   std::move(stop_evidence_support)};
           }),
           py::arg("provider_contract_ref"), py::arg("stop_command_support") = std::nullopt,
           py::arg("stop_evidence_support") = std::nullopt)
      .def_readonly("provider_contract_ref", &ProviderContractDeclarationDTO::provider_contract_ref)
      .def_readonly("stop_command_support", &ProviderContractDeclarationDTO::stop_command_support)
      .def_readonly("stop_evidence_support", &ProviderContractDeclarationDTO::stop_evidence_support)
      .def(
          "__eq__",
          [](const ProviderContractDeclarationDTO& lhs, const ProviderContractDeclarationDTO& rhs) {
            return lhs == rhs;
          },
          py::is_operator())
      .def(
          "__ne__",
          [](const ProviderContractDeclarationDTO& lhs, const ProviderContractDeclarationDTO& rhs) {
            return lhs != rhs;
          },
          py::is_operator());

  py::class_<ProviderLifecyclePlanCatalogDeclarationDTO>(
      module, "ProviderLifecyclePlanCatalogDeclarationDTO", py::is_final())
      .def(py::init<SchemaId, ContentDigest>(), py::arg("catalog_api_version"),
           py::arg("semantic_digest"))
      .def_readonly("catalog_api_version",
                    &ProviderLifecyclePlanCatalogDeclarationDTO::catalog_api_version)
      .def_readonly("semantic_digest", &ProviderLifecyclePlanCatalogDeclarationDTO::semantic_digest)
      .def(
          "__eq__",
          [](const ProviderLifecyclePlanCatalogDeclarationDTO& lhs,
             const ProviderLifecyclePlanCatalogDeclarationDTO& rhs) { return lhs == rhs; },
          py::is_operator())
      .def(
          "__ne__",
          [](const ProviderLifecyclePlanCatalogDeclarationDTO& lhs,
             const ProviderLifecyclePlanCatalogDeclarationDTO& rhs) { return lhs != rhs; },
          py::is_operator());

  py::class_<ProviderManifestDTO>(module, "ProviderManifestDTO", py::is_final())
      .def(
          py::init(
              [](ProviderManifestId provider_manifest_id,
                 ProviderManifestRevision provider_manifest_revision,
                 std::vector<ProviderCapabilityOfferDTO> offered_capabilities,
                 std::vector<ProviderContractDeclarationDTO> provider_contracts,
                 std::optional<ProviderLifecyclePlanCatalogDeclarationDTO> lifecycle_plan_catalog) {
                return ProviderManifestDTO{
                    std::move(provider_manifest_id), std::move(provider_manifest_revision),
                    std::move(offered_capabilities), std::move(provider_contracts),
                    std::move(lifecycle_plan_catalog)};
              }),
          py::arg("provider_manifest_id"), py::arg("provider_manifest_revision"),
          py::arg("offered_capabilities"), py::arg("provider_contracts"),
          py::arg("lifecycle_plan_catalog") = std::nullopt)
      .def_readonly("provider_manifest_id", &ProviderManifestDTO::provider_manifest_id)
      .def_readonly("provider_manifest_revision", &ProviderManifestDTO::provider_manifest_revision)
      .def_readonly("offered_capabilities", &ProviderManifestDTO::offered_capabilities)
      .def_readonly("provider_contracts", &ProviderManifestDTO::provider_contracts)
      .def_readonly("lifecycle_plan_catalog", &ProviderManifestDTO::lifecycle_plan_catalog)
      .def(
          "__eq__",
          [](const ProviderManifestDTO& lhs, const ProviderManifestDTO& rhs) { return lhs == rhs; },
          py::is_operator())
      .def(
          "__ne__",
          [](const ProviderManifestDTO& lhs, const ProviderManifestDTO& rhs) { return lhs != rhs; },
          py::is_operator());

  module.attr("PROVIDER_MANIFEST_API_VERSION") = std::string(kProviderManifestApiVersion);
  module.attr("MAX_PROVIDER_MANIFEST_CAPABILITIES") = kMaxProviderManifestCapabilities;
  module.attr("MAX_PROVIDER_MANIFEST_CONTRACTS") = kMaxProviderManifestContracts;
  module.attr("MAX_PROVIDER_MANIFEST_ENDPOINT_ROLES") = kMaxProviderManifestEndpointRoles;

  py::class_<StopContainmentCommandDTO>(module, "StopContainmentCommandDTO", py::is_final())
      .def(py::init<StopBudgetPhaseId, StopContainmentCommandKind,
                    std::optional<ProviderContractRefDTO>,
                    std::optional<RegisteredStopCommandKindId>>(),
           py::arg("command_phase_id"), py::arg("command_kind"),
           py::arg("provider_contract_ref") = std::nullopt,
           py::arg("registered_command_kind_id") = std::nullopt)
      .def_readonly("command_phase_id", &StopContainmentCommandDTO::command_phase_id)
      .def_readonly("command_kind", &StopContainmentCommandDTO::command_kind)
      .def_readonly("provider_contract_ref", &StopContainmentCommandDTO::provider_contract_ref)
      .def_readonly("registered_command_kind_id",
                    &StopContainmentCommandDTO::registered_command_kind_id)
      .def(
          "__eq__",
          [](const StopContainmentCommandDTO& lhs, const StopContainmentCommandDTO& rhs) {
            return lhs == rhs;
          },
          py::is_operator());

  py::class_<StopBudgetPhaseDTO>(module, "StopBudgetPhaseDTO", py::is_final())
      .def(py::init<StopBudgetPhaseId, std::int64_t>(), py::arg("phase_id"),
           py::arg("deadline_offset_ns"))
      .def_readonly("phase_id", &StopBudgetPhaseDTO::phase_id)
      .def_readonly("deadline_offset_ns", &StopBudgetPhaseDTO::deadline_offset_ns)
      .def(
          "__eq__",
          [](const StopBudgetPhaseDTO& lhs, const StopBudgetPhaseDTO& rhs) { return lhs == rhs; },
          py::is_operator());

  py::class_<StopTimingParametersDTO>(module, "StopTimingParametersDTO", py::is_final())
      .def(py::init<std::int64_t, std::int64_t, std::int64_t>(),
           py::arg("command_latency_allowance_ns"), py::arg("settling_allowance_ns"),
           py::arg("operational_timeout_ns"))
      .def_readonly("command_latency_allowance_ns",
                    &StopTimingParametersDTO::command_latency_allowance_ns)
      .def_readonly("settling_allowance_ns", &StopTimingParametersDTO::settling_allowance_ns)
      .def_readonly("operational_timeout_ns", &StopTimingParametersDTO::operational_timeout_ns)
      .def(py::self == py::self);

  module.def(
      "authored_stop_duration_ns",
      [](double seconds) {
        const auto result = authored_stop_duration_ns(seconds);
        if (!result.ok()) throw py::value_error(result.error().message);
        return result.value();
      },
      py::arg("seconds"));
  module.def(
      "validate_controlled_stop_timing",
      [](const StopTimingParametersDTO& timing, std::int64_t duration) -> std::optional<ErrorDTO> {
        const auto result = validate_controlled_stop_timing(timing, duration);
        return result.ok() ? std::nullopt : std::optional(result.error());
      },
      py::arg("timing"), py::arg("full_profile_duration_ns"));
  module.def(
      "resolve_stop_timing_budget",
      [](const StopEvidencePolicyDTO& policy,
         const std::optional<StopTimingParametersDTO>& timing) {
        auto result = resolve_stop_timing_budget(policy, timing);
        if (!result.ok()) throw py::value_error(result.error().message);
        return std::move(result.value());
      },
      py::arg("policy"), py::arg("timing") = std::nullopt);
  module.def(
      "harmonize_stop_timing_budgets",
      [](std::vector<StopDeadlineBudgetDTO> budgets) {
        auto result = harmonize_stop_timing_budgets(budgets);
        if (!result.ok()) throw py::value_error(result.error().message);
        return budgets;
      },
      py::arg("budgets"));

  py::class_<StopDeadlineBudgetDTO>(module, "StopDeadlineBudgetDTO", py::is_final())
      .def(py::init<std::int64_t, std::int64_t, std::vector<StopBudgetPhaseDTO>>(),
           py::arg("outer_duration_ns"), py::arg("kernel_completion_reserve_ns"), py::arg("phases"))
      .def_readonly("outer_duration_ns", &StopDeadlineBudgetDTO::outer_duration_ns)
      .def_readonly("kernel_completion_reserve_ns",
                    &StopDeadlineBudgetDTO::kernel_completion_reserve_ns)
      .def_readonly("phases", &StopDeadlineBudgetDTO::phases)
      .def(
          "__eq__",
          [](const StopDeadlineBudgetDTO& lhs, const StopDeadlineBudgetDTO& rhs) {
            return lhs == rhs;
          },
          py::is_operator());

  py::class_<StopEvidenceSourceRequirementDTO>(module, "StopEvidenceSourceRequirementDTO",
                                               py::is_final())
      .def(py::init<StopEvidenceSourceKind, std::int64_t, std::optional<EndpointRoleId>,
                    std::vector<StateAuthorityClass>>(),
           py::arg("source_kind"), py::arg("freshness_budget_ns"),
           py::arg("endpoint_role_id") = std::nullopt,
           py::arg("allowed_authority_classes") = std::vector<StateAuthorityClass>{})
      .def_readonly("source_kind", &StopEvidenceSourceRequirementDTO::source_kind)
      .def_readonly("freshness_budget_ns", &StopEvidenceSourceRequirementDTO::freshness_budget_ns)
      .def_readonly("endpoint_role_id", &StopEvidenceSourceRequirementDTO::endpoint_role_id)
      .def_readonly("allowed_authority_classes",
                    &StopEvidenceSourceRequirementDTO::allowed_authority_classes)
      .def(
          "__eq__",
          [](const StopEvidenceSourceRequirementDTO& lhs,
             const StopEvidenceSourceRequirementDTO& rhs) { return lhs == rhs; },
          py::is_operator());

  py::class_<StopEvidenceObservationDTO>(module, "StopEvidenceObservationDTO", py::is_final())
      .def(py::init<StopEvidenceObservationKind, std::optional<std::int64_t>>(),
           py::arg("observation_kind"), py::arg("minimum_dwell_ns") = std::nullopt)
      .def_readonly("observation_kind", &StopEvidenceObservationDTO::observation_kind)
      .def_readonly("minimum_dwell_ns", &StopEvidenceObservationDTO::minimum_dwell_ns)
      .def(
          "__eq__",
          [](const StopEvidenceObservationDTO& lhs, const StopEvidenceObservationDTO& rhs) {
            return lhs == rhs;
          },
          py::is_operator());

  py::class_<StopEvidenceRequirementDTO>(module, "StopEvidenceRequirementDTO", py::is_final())
      .def(py::init([](StopEvidenceItemId evidence_item_id, StopBudgetPhaseId phase_id,
                       StopEvidenceRequirementKind evidence_kind,
                       StopEvidenceSourceRequirementDTO source_requirement,
                       StopEvidenceObservationDTO observation_semantics,
                       std::optional<ProviderContractRefDTO> provider_contract_ref,
                       std::optional<RegisteredStopEvidenceKindId> registered_evidence_kind_id) {
             return StopEvidenceRequirementDTO{std::move(evidence_item_id),
                                               std::move(phase_id),
                                               evidence_kind,
                                               std::move(provider_contract_ref),
                                               std::move(registered_evidence_kind_id),
                                               std::move(source_requirement),
                                               std::move(observation_semantics)};
           }),
           py::arg("evidence_item_id"), py::arg("phase_id"), py::arg("evidence_kind"),
           py::arg("source_requirement"), py::arg("observation_semantics"),
           py::arg("provider_contract_ref") = std::nullopt,
           py::arg("registered_evidence_kind_id") = std::nullopt)
      .def_readonly("evidence_item_id", &StopEvidenceRequirementDTO::evidence_item_id)
      .def_readonly("phase_id", &StopEvidenceRequirementDTO::phase_id)
      .def_readonly("evidence_kind", &StopEvidenceRequirementDTO::evidence_kind)
      .def_readonly("provider_contract_ref", &StopEvidenceRequirementDTO::provider_contract_ref)
      .def_readonly("registered_evidence_kind_id",
                    &StopEvidenceRequirementDTO::registered_evidence_kind_id)
      .def_readonly("source_requirement", &StopEvidenceRequirementDTO::source_requirement)
      .def_readonly("observation_semantics", &StopEvidenceRequirementDTO::observation_semantics)
      .def(
          "__eq__",
          [](const StopEvidenceRequirementDTO& lhs, const StopEvidenceRequirementDTO& rhs) {
            return lhs == rhs;
          },
          py::is_operator());

  py::class_<StopEscalationStageDTO>(module, "StopEscalationStageDTO", py::is_final())
      .def(py::init<StopEscalationStageId, StopEscalationStageKind, ContainmentGranularity,
                    StopContainmentCommandDTO, StopEvidenceConjunction,
                    std::vector<StopEvidenceRequirementDTO>>(),
           py::arg("stop_escalation_stage_id"), py::arg("stage_kind"),
           py::arg("containment_granularity"), py::arg("containment_command"),
           py::arg("evidence_conjunction"), py::arg("evidence_requirements"))
      .def_readonly("stop_escalation_stage_id", &StopEscalationStageDTO::stop_escalation_stage_id)
      .def_readonly("stage_kind", &StopEscalationStageDTO::stage_kind)
      .def_readonly("containment_granularity", &StopEscalationStageDTO::containment_granularity)
      .def_readonly("containment_command", &StopEscalationStageDTO::containment_command)
      .def_readonly("evidence_conjunction", &StopEscalationStageDTO::evidence_conjunction)
      .def_readonly("evidence_requirements", &StopEscalationStageDTO::evidence_requirements)
      .def(
          "__eq__",
          [](const StopEscalationStageDTO& lhs, const StopEscalationStageDTO& rhs) {
            return lhs == rhs;
          },
          py::is_operator());

  py::class_<StopEvidencePolicyDTO>(module, "StopEvidencePolicyDTO", py::is_final())
      .def(py::init<StopEvidencePolicyId, StopEvidencePolicyRevision, StopEvidenceDisposition,
                    ContainmentGranularity, std::optional<StopDeadlineBudgetDTO>,
                    std::vector<StopEscalationStageDTO>>(),
           py::arg("stop_evidence_policy_id"), py::arg("stop_evidence_policy_revision"),
           py::arg("disposition"), py::arg("containment_granularity"),
           py::arg("deadline_budget") = std::nullopt,
           py::arg("escalation_stages") = std::vector<StopEscalationStageDTO>{})
      .def_readonly("stop_evidence_policy_id", &StopEvidencePolicyDTO::stop_evidence_policy_id)
      .def_readonly("stop_evidence_policy_revision",
                    &StopEvidencePolicyDTO::stop_evidence_policy_revision)
      .def_readonly("disposition", &StopEvidencePolicyDTO::disposition)
      .def_readonly("containment_granularity", &StopEvidencePolicyDTO::containment_granularity)
      .def_readonly("deadline_budget", &StopEvidencePolicyDTO::deadline_budget)
      .def_readonly("escalation_stages", &StopEvidencePolicyDTO::escalation_stages)
      .def(
          "__eq__",
          [](const StopEvidencePolicyDTO& lhs, const StopEvidencePolicyDTO& rhs) {
            return lhs == rhs;
          },
          py::is_operator());

  module.attr("STOP_EVIDENCE_POLICY_API_VERSION") = std::string(kStopEvidencePolicyApiVersion);
  module.attr("MAX_STOP_ESCALATION_STAGES") = kMaxStopEscalationStages;
  module.attr("MAX_STOP_EVIDENCE_POLICY_ITEMS") = kMaxStopEvidencePolicyItems;
  module.attr("MAX_STOP_EVIDENCE_POLICY_DURATION_NS") = kMaxStopEvidencePolicyDurationNs;

  py::class_<ValueSchemaRefDescriptor>(module, "ValueSchemaRefDescriptor", py::is_final())
      .def_property_readonly(
          "schema_id",
          [](const ValueSchemaRefDescriptor& value) { return std::string(value.schema_id); })
      .def_property_readonly("schema_revision", [](const ValueSchemaRefDescriptor& value) {
        return std::string(value.schema_revision);
      });

  py::class_<CapabilityDefinitionDescriptor>(module, "CapabilityDefinitionDescriptor",
                                             py::is_final())
      .def_property_readonly("capability_id",
                             [](const CapabilityDefinitionDescriptor& value) {
                               return std::string(value.capability_id);
                             })
      .def_property_readonly("capability_definition_revision",
                             [](const CapabilityDefinitionDescriptor& value) {
                               return std::string(value.capability_definition_revision);
                             })
      .def_readonly("scope", &CapabilityDefinitionDescriptor::scope)
      .def_readonly("provider_binding_kind", &CapabilityDefinitionDescriptor::provider_binding_kind)
      .def_readonly("requirement_parameters_schema_ref",
                    &CapabilityDefinitionDescriptor::requirement_parameters_schema_ref);

  module.attr("CAPABILITY_DEFINITION_API_VERSION") = std::string(kCapabilityDefinitionApiVersion);
  module.attr("BUILTIN_CAPABILITY_DEFINITION_REGISTRY_DIGEST") =
      std::string(kBuiltinCapabilityDefinitionRegistryDigest);
  py::tuple capability_registry{kBuiltinCapabilityDefinitionRegistry.size()};
  for (std::size_t index = 0U; index < kBuiltinCapabilityDefinitionRegistry.size(); ++index) {
    capability_registry[index] = py::cast(kBuiltinCapabilityDefinitionRegistry[index]);
  }
  module.attr("BUILTIN_CAPABILITY_DEFINITION_REGISTRY") = capability_registry;
  module.def(
      "find_builtin_capability_definition",
      [](std::string_view capability_id) -> std::optional<CapabilityDefinitionDescriptor> {
        const auto* found = manyforge::core::find_builtin_capability_definition(capability_id);
        return found == nullptr ? std::nullopt
                                : std::optional<CapabilityDefinitionDescriptor>(*found);
      },
      py::arg("capability_id"));
  module.def("materialize_capability_definition",
             &manyforge::core::materialize_capability_definition, py::arg("descriptor"));
  module.def(
      "capability_scope_to_string",
      [](CapabilityScope scope) { return std::string(to_string(scope)); }, py::arg("scope"));
  module.def(
      "capability_provider_binding_kind_to_string",
      [](CapabilityProviderBindingKind kind) { return std::string(to_string(kind)); },
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
      .value("kStopEvidencePolicy", CatalogRecordKind::kStopEvidencePolicy);

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

  constexpr std::array<const char*, 10> kRecordRefTypeNames{{
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
}

}  // namespace manyforge::core::python
