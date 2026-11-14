#include <pybind11/operators.h>
#include <pybind11/pybind11.h>

#include <manyforge_core/types/request_key.hpp>
#include <manyforge_core/types/retained_product.hpp>
#include <manyforge_core/types/strong_id.hpp>
#include <string>

#include "binding_registrations.hpp"

namespace py = pybind11;

namespace manyforge::core::python {
namespace {

template <typename StrongType>
void bind_strong_string(py::module_& module, const char* python_name) {
  const std::string type_name{python_name};
  py::class_<StrongType>(module, python_name, py::is_final())
      .def(py::init<std::string>(), py::arg("value"))
      .def_property_readonly("value", &StrongType::value)
      .def("__str__", [](const StrongType& value) { return value.value(); })
      .def("__repr__",
           [type_name](const StrongType& value) {
             return py::str("{}({})").format(type_name, py::repr(py::str(value.value())));
           })
      .def(
          "__eq__", [](const StrongType& lhs, const StrongType& rhs) { return lhs == rhs; },
          py::is_operator())
      .def(
          "__ne__", [](const StrongType& lhs, const StrongType& rhs) { return lhs != rhs; },
          py::is_operator())
      .def("__hash__", [type_name](const StrongType& value) {
        return py::hash(py::make_tuple(type_name, value.value()));
      });
}

template <typename StrongType>
void bind_strong_counter(py::module_& module, const char* python_name) {
  const std::string type_name{python_name};
  py::class_<StrongType>(module, python_name, py::is_final())
      .def(py::init<std::uint64_t>(), py::arg("value"))
      .def_property_readonly("value", &StrongType::value)
      .def_property_readonly("nonzero", &StrongType::nonzero)
      .def("__repr__",
           [type_name](const StrongType& value) {
             return py::str("{}({})").format(type_name, value.value());
           })
      .def(
          "__eq__", [](const StrongType& lhs, const StrongType& rhs) { return lhs == rhs; },
          py::is_operator())
      .def(
          "__ne__", [](const StrongType& lhs, const StrongType& rhs) { return lhs != rhs; },
          py::is_operator())
      .def("__hash__", [type_name](const StrongType& value) {
        return py::hash(py::make_tuple(type_name, value.value()));
      });
}

// One operation participant (contract 1.5): (operation number, ordinal).
void bind_operation_participant_id(py::module_& module) {
  py::class_<OperationParticipantId>(module, "OperationParticipantId", py::is_final())
      .def(py::init([](OperationNumber operation_id, std::uint32_t ordinal) {
             return OperationParticipantId{operation_id, ordinal};
           }),
           py::arg("operation_id"), py::arg("ordinal"))
      .def_readonly("operation_id", &OperationParticipantId::operation_id)
      .def_readonly("ordinal", &OperationParticipantId::ordinal)
      .def("__repr__",
           [](const OperationParticipantId& value) {
             return py::str("OperationParticipantId({}, {})")
                 .format(value.operation_id.value(), value.ordinal);
           })
      .def(
          "__eq__",
          [](const OperationParticipantId& lhs, const OperationParticipantId& rhs) {
            return lhs == rhs;
          },
          py::is_operator())
      .def(
          "__ne__",
          [](const OperationParticipantId& lhs, const OperationParticipantId& rhs) {
            return lhs != rhs;
          },
          py::is_operator())
      .def("__hash__", [](const OperationParticipantId& value) {
        return py::hash(
            py::make_tuple("OperationParticipantId", value.operation_id.value(), value.ordinal));
      });
}

// The 16-byte request key (contract 1.4): `bytes` of length 16 in Python,
// 32 lowercase hex characters on every wire.
void bind_request_key(py::module_& module) {
  py::class_<RequestKey>(module, "RequestKey", py::is_final())
      .def(py::init([](const py::bytes& value) {
             const std::string raw = value;
             if (raw.size() != 16U) throw py::value_error("a request key is exactly 16 bytes");
             RequestKey key;
             std::copy(raw.begin(), raw.end(), key.bytes.begin());
             return key;
           }),
           py::arg("value"))
      .def_static(
          "from_hex",
          [](const std::string& text) {
            const auto key = ParseRequestKey(text);
            if (!key.has_value()) throw py::value_error("a request key is 32 hex characters");
            return *key;
          },
          py::arg("text"))
      .def_property_readonly("value",
                             [](const RequestKey& key) {
                               return py::bytes(reinterpret_cast<const char*>(key.bytes.data()),
                                                key.bytes.size());
                             })
      .def("hex", [](const RequestKey& key) { return RequestKeyText(key); })
      .def("__repr__",
           [](const RequestKey& key) { return "RequestKey.from_hex('" + RequestKeyText(key) + "')"; })
      .def(
          "__eq__", [](const RequestKey& lhs, const RequestKey& rhs) { return lhs == rhs; },
          py::is_operator())
      .def(
          "__ne__", [](const RequestKey& lhs, const RequestKey& rhs) { return lhs != rhs; },
          py::is_operator())
      .def("__hash__", [](const RequestKey& key) {
        return py::hash(py::make_tuple("RequestKey", RequestKeyText(key)));
      });
}
}  // namespace

void register_strong_identity_bindings(py::module_& module) {
#define MANYFORGE_BIND_STRONG_STRING(Name) bind_strong_string<Name>(module, #Name)

  MANYFORGE_BIND_STRONG_STRING(CellId);
  MANYFORGE_BIND_STRONG_STRING(GraphRevision);
  MANYFORGE_BIND_STRONG_STRING(SchemaId);
  MANYFORGE_BIND_STRONG_STRING(SchemaRevision);
  MANYFORGE_BIND_STRONG_STRING(ContentDigest);
  MANYFORGE_BIND_STRONG_STRING(ContentAddressedStoreKey);

  MANYFORGE_BIND_STRONG_STRING(RuntimeProfileId);
  MANYFORGE_BIND_STRONG_STRING(RuntimeProfileRevision);
  MANYFORGE_BIND_STRONG_STRING(RuntimeRoleId);
  MANYFORGE_BIND_STRONG_STRING(RuntimeUnitId);
  MANYFORGE_BIND_STRONG_STRING(RuntimeEpoch);
  MANYFORGE_BIND_STRONG_STRING(RuntimePolicyId);
  MANYFORGE_BIND_STRONG_STRING(RuntimePolicyRevision);
  MANYFORGE_BIND_STRONG_STRING(InterruptSourceId);
  MANYFORGE_BIND_STRONG_STRING(InterruptPolicyId);
  MANYFORGE_BIND_STRONG_STRING(MotionInterruptScopeId);

  MANYFORGE_BIND_STRONG_STRING(ComponentTypeId);
  MANYFORGE_BIND_STRONG_STRING(ComponentTypeRevision);
  MANYFORGE_BIND_STRONG_STRING(ControllableComponentId);
  MANYFORGE_BIND_STRONG_STRING(ModelAssetId);
  MANYFORGE_BIND_STRONG_STRING(ModelInstanceId);
  MANYFORGE_BIND_STRONG_STRING(ModelRevision);
  MANYFORGE_BIND_STRONG_STRING(ModelRoleId);
  MANYFORGE_BIND_STRONG_STRING(ModelGroupRoleId);
  MANYFORGE_BIND_STRONG_STRING(ComponentRoleId);
  MANYFORGE_BIND_STRONG_STRING(CommandTargetId);
  MANYFORGE_BIND_STRONG_STRING(TargetRoleId);
  MANYFORGE_BIND_STRONG_STRING(CapabilityId);
  MANYFORGE_BIND_STRONG_STRING(CapabilityDefinitionRevision);
  MANYFORGE_BIND_STRONG_STRING(FrameRoleId);
  MANYFORGE_BIND_STRONG_STRING(CollisionBodyRoleId);
  MANYFORGE_BIND_STRONG_STRING(ResourceId);
  MANYFORGE_BIND_STRONG_STRING(ResourceRoleId);
  MANYFORGE_BIND_STRONG_STRING(AttachmentInterfaceId);

  MANYFORGE_BIND_STRONG_STRING(ControlInterfaceContractId);
  MANYFORGE_BIND_STRONG_STRING(ControlInterfaceRevision);
  MANYFORGE_BIND_STRONG_STRING(ControllerTopologyId);
  MANYFORGE_BIND_STRONG_STRING(TopologyRevision);
  MANYFORGE_BIND_STRONG_STRING(ControllerRoleId);
  MANYFORGE_BIND_STRONG_STRING(ActivationSetId);
  MANYFORGE_BIND_STRONG_STRING(ControlRouteId);
  MANYFORGE_BIND_STRONG_STRING(ScalingDomainId);
  MANYFORGE_BIND_STRONG_STRING(CoordinatedScalingBarrierId);
  MANYFORGE_BIND_STRONG_STRING(ControlDomainId);
  MANYFORGE_BIND_STRONG_STRING(ControlDomainRevision);
  MANYFORGE_BIND_STRONG_STRING(ControlBindingId);
  MANYFORGE_BIND_STRONG_STRING(ControlBindingRevision);
  MANYFORGE_BIND_STRONG_STRING(ActiveControlConfigurationId);
  MANYFORGE_BIND_STRONG_STRING(ResolvedStopBasePlanId);
  MANYFORGE_BIND_STRONG_STRING(EndpointRoleId);
  MANYFORGE_BIND_STRONG_STRING(StateAuthorityId);
  MANYFORGE_BIND_STRONG_STRING(StopEvidencePolicyId);
  MANYFORGE_BIND_STRONG_STRING(StopEvidencePolicyRevision);
  MANYFORGE_BIND_STRONG_STRING(StopEscalationStageId);
  MANYFORGE_BIND_STRONG_STRING(StopEvidenceItemId);
  MANYFORGE_BIND_STRONG_STRING(ProviderContractRoleId);
  MANYFORGE_BIND_STRONG_STRING(RegisteredStopCommandKindId);
  MANYFORGE_BIND_STRONG_STRING(RegisteredStopEvidenceKindId);
  MANYFORGE_BIND_STRONG_STRING(CommissionedStopInterfaceId);
  MANYFORGE_BIND_STRONG_STRING(CommissionedStopInterfaceRevision);
  MANYFORGE_BIND_STRONG_STRING(StopInvocationLaneId);
  MANYFORGE_BIND_STRONG_STRING(ContactPolicyId);
  MANYFORGE_BIND_STRONG_STRING(ContactPolicyRevision);

  MANYFORGE_BIND_STRONG_STRING(ProviderInstanceId);
  MANYFORGE_BIND_STRONG_STRING(ProviderManifestId);
  MANYFORGE_BIND_STRONG_STRING(ProviderManifestRevision);
  MANYFORGE_BIND_STRONG_STRING(CapabilityProviderBindingId);
  MANYFORGE_BIND_STRONG_STRING(CapabilityProviderBindingRevision);
  MANYFORGE_BIND_STRONG_STRING(SiteOverlayId);
  MANYFORGE_BIND_STRONG_STRING(SiteOverlayRevision);
  MANYFORGE_BIND_STRONG_STRING(ExternalResourceAlias);
  MANYFORGE_BIND_STRONG_STRING(SiteOverlayValueRef);

  MANYFORGE_BIND_STRONG_STRING(DeploymentArtifactId);
  MANYFORGE_BIND_STRONG_STRING(DeploymentRevision);
  MANYFORGE_BIND_STRONG_STRING(ProductId);
  MANYFORGE_BIND_STRONG_STRING(ProductContractId);
  MANYFORGE_BIND_STRONG_STRING(ProductContractRevision);
  MANYFORGE_BIND_STRONG_STRING(ArtifactRoleId);

  MANYFORGE_BIND_STRONG_STRING(ComponentAttachmentId);
  MANYFORGE_BIND_STRONG_STRING(ComponentAttachmentRevision);

  MANYFORGE_BIND_STRONG_STRING(RetainedBundleId);
  MANYFORGE_BIND_STRONG_STRING(SceneId);
  MANYFORGE_BIND_STRONG_STRING(ProjectionLeaseId);
  MANYFORGE_BIND_STRONG_STRING(BootstrapId);

  MANYFORGE_BIND_STRONG_STRING(CommandSchemaId);
  MANYFORGE_BIND_STRONG_STRING(CommandSchemaRevision);
  MANYFORGE_BIND_STRONG_STRING(CommandOptionId);
  MANYFORGE_BIND_STRONG_STRING(ChannelRoleId);
  MANYFORGE_BIND_STRONG_STRING(ProgramId);
  MANYFORGE_BIND_STRONG_STRING(ProgramRevision);
  MANYFORGE_BIND_STRONG_STRING(ProgramBindingId);
  MANYFORGE_BIND_STRONG_STRING(MoveId);

  MANYFORGE_BIND_STRONG_STRING(LifecycleTransactionId);
  MANYFORGE_BIND_STRONG_STRING(LifecycleFactId);
  MANYFORGE_BIND_STRONG_STRING(ConfigurationFenceId);

#undef MANYFORGE_BIND_STRONG_STRING
  bind_request_key(module);

  bind_strong_counter<ActiveControlConfigurationGeneration>(module,
                                                            "ActiveControlConfigurationGeneration");
  bind_strong_counter<ProjectionStageGeneration>(module, "ProjectionStageGeneration");
  bind_strong_counter<ActiveBundlePublicationGeneration>(module,
                                                         "ActiveBundlePublicationGeneration");
  bind_strong_counter<BootstrapGeneration>(module, "BootstrapGeneration");
  bind_strong_counter<ApplyId>(module, "ApplyId");
  bind_strong_counter<ApplyGeneration>(module, "ApplyGeneration");
  bind_strong_counter<LifecycleTransactionGeneration>(module, "LifecycleTransactionGeneration");
  bind_strong_counter<ComponentLifecycleGeneration>(module, "ComponentLifecycleGeneration");
  bind_strong_counter<OperationRevision>(module, "OperationRevision");
  bind_strong_counter<AdmissionGeneration>(module, "AdmissionGeneration");
  bind_strong_counter<CellAdmissionClaimGeneration>(module, "CellAdmissionClaimGeneration");
  bind_strong_counter<ConfigurationFenceGeneration>(module, "ConfigurationFenceGeneration");
  bind_strong_counter<StopEventId>(module, "StopEventId");
  bind_strong_counter<OperationNumber>(module, "OperationNumber");
  bind_strong_counter<ProgramRunNumber>(module, "ProgramRunNumber");
  bind_strong_counter<NodeAttemptNumber>(module, "NodeAttemptNumber");
  bind_operation_participant_id(module);
}

}  // namespace manyforge::core::python
