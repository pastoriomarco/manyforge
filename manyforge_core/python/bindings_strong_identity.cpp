#include <pybind11/operators.h>
#include <pybind11/pybind11.h>

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
      .def("__repr__", [type_name](const StrongType& value) {
        return py::str("{}({})").format(type_name, value.value());
      })
      .def("__eq__",
           [](const StrongType& lhs, const StrongType& rhs) {
             return lhs == rhs;
           },
           py::is_operator())
      .def("__ne__",
           [](const StrongType& lhs, const StrongType& rhs) {
             return lhs != rhs;
           },
           py::is_operator())
      .def("__hash__", [type_name](const StrongType& value) {
        return py::hash(py::make_tuple(type_name, value.value()));
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
  MANYFORGE_BIND_STRONG_STRING(StopBudgetPhaseId);
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

  MANYFORGE_BIND_STRONG_STRING(CommandSchemaId);
  MANYFORGE_BIND_STRONG_STRING(CommandSchemaRevision);
  MANYFORGE_BIND_STRONG_STRING(CommandOptionId);
  MANYFORGE_BIND_STRONG_STRING(ChannelRoleId);
  MANYFORGE_BIND_STRONG_STRING(ProgramId);
  MANYFORGE_BIND_STRONG_STRING(ProgramRevision);
  MANYFORGE_BIND_STRONG_STRING(ProgramBindingId);
  MANYFORGE_BIND_STRONG_STRING(ProgramRunId);
  MANYFORGE_BIND_STRONG_STRING(MoveId);

#undef MANYFORGE_BIND_STRONG_STRING

  bind_strong_counter<ActiveControlConfigurationGeneration>(
      module, "ActiveControlConfigurationGeneration");
}

}  // namespace manyforge::core::python
