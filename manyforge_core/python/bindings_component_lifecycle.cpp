#include <pybind11/operators.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <manyforge_core/types/component_lifecycle.hpp>

#include "binding_registrations.hpp"

namespace py = pybind11;

namespace manyforge::core::python {
namespace {

template <typename Value>
py::tuple immutable_snapshot(const std::vector<Value>& values) {
  py::tuple result(values.size());
  for (std::size_t index = 0U; index < values.size(); ++index) {
    result[index] = py::cast(values[index]);
  }
  return result;
}

}  // namespace

void register_component_lifecycle_bindings(py::module_& module) {
  py::enum_<RequestReplayStatusDTO>(module, "RequestReplayStatusDTO")
      .value("kAcceptedNew", RequestReplayStatusDTO::kAcceptedNew)
      .value("kReplayedExact", RequestReplayStatusDTO::kReplayedExact)
      .value("kRequestResultExpired", RequestReplayStatusDTO::kRequestResultExpired)
      .value("kRequestIdConflict", RequestReplayStatusDTO::kRequestIdConflict)
      .value("kRequestLedgerFull", RequestReplayStatusDTO::kRequestLedgerFull);

  py::enum_<ComponentLifecycleStateDTO>(module, "ComponentLifecycleStateDTO")
      .value("kUndeclared", ComponentLifecycleStateDTO::kUndeclared)
      .value("kDeclared", ComponentLifecycleStateDTO::kDeclared)
      .value("kPrepared", ComponentLifecycleStateDTO::kPrepared)
      .value("kActive", ComponentLifecycleStateDTO::kActive)
      .value("kDegraded", ComponentLifecycleStateDTO::kDegraded)
      .value("kFaulted", ComponentLifecycleStateDTO::kFaulted)
      .value("kDisabling", ComponentLifecycleStateDTO::kDisabling)
      .value("kDisabled", ComponentLifecycleStateDTO::kDisabled);

  py::enum_<ConfigurationFenceState>(module, "ConfigurationFenceState")
      .value("kHeld", ConfigurationFenceState::kHeld)
      .value("kReleased", ConfigurationFenceState::kReleased);

  py::class_<NamedApplyPublicationTransactionDTO>(module, "NamedApplyPublicationTransactionDTO",
                                                  py::is_final())
      .def(py::init<ApplyId, ApplyGeneration>(), py::arg("apply_id"),
           py::arg("apply_generation"))
      .def_readonly("apply_id", &NamedApplyPublicationTransactionDTO::apply_id)
      .def_readonly("apply_generation", &NamedApplyPublicationTransactionDTO::apply_generation)
      .def("valid", &NamedApplyPublicationTransactionDTO::valid)
      .def(py::self == py::self)
      .def(py::self != py::self);

  py::class_<BootstrapPublicationTransactionDTO>(module, "BootstrapPublicationTransactionDTO",
                                                 py::is_final())
      .def(py::init<BootstrapId, BootstrapGeneration>(), py::arg("bootstrap_id"),
           py::arg("bootstrap_generation"))
      .def_readonly("bootstrap_id", &BootstrapPublicationTransactionDTO::bootstrap_id)
      .def_readonly("bootstrap_generation",
                    &BootstrapPublicationTransactionDTO::bootstrap_generation)
      .def("valid", &BootstrapPublicationTransactionDTO::valid)
      .def(py::self == py::self)
      .def(py::self != py::self);

  py::class_<ActiveBundlePublicationRequestDTO>(module, "ActiveBundlePublicationRequestDTO",
                                                py::is_final())
      .def(py::init([](NamedApplyPublicationTransactionDTO transaction,
                       ProjectionRevisionDTO candidate_projection_revision,
                       RetainedBundleId candidate_retained_bundle_id,
                       GraphRevision candidate_graph_revision,
                       std::optional<ActiveBundlePublicationGeneration>
                           expected_previous_publication_generation,
                       ContentDigest private_materialization_content_digest) {
             return ActiveBundlePublicationRequestDTO{
                 ActiveBundlePublicationTransactionRefDTO(std::move(transaction)),
                 std::move(candidate_projection_revision),
                 std::move(candidate_retained_bundle_id),
                 std::move(candidate_graph_revision),
                 expected_previous_publication_generation,
                 std::move(private_materialization_content_digest)};
           }),
           py::arg("transaction_ref"), py::arg("candidate_projection_revision"),
           py::arg("candidate_retained_bundle_id"), py::arg("candidate_graph_revision"),
           py::arg("expected_previous_publication_generation"),
           py::arg("private_materialization_content_digest"))
      .def(py::init([](BootstrapPublicationTransactionDTO transaction,
                       ProjectionRevisionDTO candidate_projection_revision,
                       RetainedBundleId candidate_retained_bundle_id,
                       GraphRevision candidate_graph_revision,
                       std::optional<ActiveBundlePublicationGeneration>
                           expected_previous_publication_generation,
                       ContentDigest private_materialization_content_digest) {
             return ActiveBundlePublicationRequestDTO{
                 ActiveBundlePublicationTransactionRefDTO(std::move(transaction)),
                 std::move(candidate_projection_revision),
                 std::move(candidate_retained_bundle_id),
                 std::move(candidate_graph_revision),
                 expected_previous_publication_generation,
                 std::move(private_materialization_content_digest)};
           }),
           py::arg("transaction_ref"), py::arg("candidate_projection_revision"),
           py::arg("candidate_retained_bundle_id"), py::arg("candidate_graph_revision"),
           py::arg("expected_previous_publication_generation"),
           py::arg("private_materialization_content_digest"))
      .def_property_readonly("transaction_ref",
                             [](const ActiveBundlePublicationRequestDTO& value) {
                               return std::visit(
                                   [](const auto& transaction) { return py::cast(transaction); },
                                   value.transaction_ref);
                             })
      .def_readonly("candidate_projection_revision",
                    &ActiveBundlePublicationRequestDTO::candidate_projection_revision)
      .def_readonly("candidate_retained_bundle_id",
                    &ActiveBundlePublicationRequestDTO::candidate_retained_bundle_id)
      .def_readonly("candidate_graph_revision",
                    &ActiveBundlePublicationRequestDTO::candidate_graph_revision)
      .def_readonly("expected_previous_publication_generation",
                    &ActiveBundlePublicationRequestDTO::expected_previous_publication_generation)
      .def_readonly("private_materialization_content_digest",
                    &ActiveBundlePublicationRequestDTO::private_materialization_content_digest)
      .def("valid", &ActiveBundlePublicationRequestDTO::valid)
      .def(py::self == py::self)
      .def(py::self != py::self);

  py::class_<ActiveBundlePublicationReceiptDTO>(module, "ActiveBundlePublicationReceiptDTO",
                                                py::is_final())
      .def(py::init<ActiveBundlePublicationRequestDTO, ActiveBundlePublicationGeneration,
                    ContentDigest>(),
           py::arg("request"), py::arg("publication_generation"),
           py::arg("durable_public_pointer_content_digest"))
      .def_readonly("request", &ActiveBundlePublicationReceiptDTO::request)
      .def_readonly("publication_generation",
                    &ActiveBundlePublicationReceiptDTO::publication_generation)
      .def_readonly("durable_public_pointer_content_digest",
                    &ActiveBundlePublicationReceiptDTO::durable_public_pointer_content_digest)
      .def("valid", &ActiveBundlePublicationReceiptDTO::valid)
      .def(py::self == py::self)
      .def(py::self != py::self);

  py::class_<PublicRequestContextDTO>(module, "PublicRequestContextDTO", py::is_final())
      .def(py::init<RequestKey, std::string>(), py::arg("request_key"), py::arg("source_label"))
      .def_readonly("request_key", &PublicRequestContextDTO::request_key)
      .def_readonly("source_label", &PublicRequestContextDTO::source_label)
      .def(py::self == py::self)
      .def(py::self != py::self);

  py::class_<ProgramNodeAdmissionEvidenceDTO>(module, "ProgramNodeAdmissionEvidenceDTO",
                                              py::is_final())
      .def(py::init<ProgramRunNumber, CellAdmissionClaimGeneration, std::string, NodeAttemptNumber,
                    std::string>(),
           py::arg("program_run_id"),
           py::arg("parent_program_cycle_claim_generation"), py::arg("node_instance_id"),
           py::arg("node_attempt_id"), py::arg("owner_api"))
      .def_readonly("program_run_id", &ProgramNodeAdmissionEvidenceDTO::program_run_id)
      .def_readonly("parent_program_cycle_claim_generation",
                    &ProgramNodeAdmissionEvidenceDTO::parent_program_cycle_claim_generation)
      .def_readonly("node_instance_id", &ProgramNodeAdmissionEvidenceDTO::node_instance_id)
      .def_readonly("node_attempt_id", &ProgramNodeAdmissionEvidenceDTO::node_attempt_id)
      .def_readonly("owner_api", &ProgramNodeAdmissionEvidenceDTO::owner_api)
      .def("locally_valid", &ProgramNodeAdmissionEvidenceDTO::locally_valid)
      .def(py::self == py::self);

  py::class_<ResolvedProviderDomainScopeDTO>(module, "ResolvedProviderDomainScopeDTO",
                                             py::is_final())
      .def(py::init<ProviderControlDomainDTO, RuntimeEpoch, std::vector<ResourceId>>(),
           py::arg("provider"), py::arg("provider_runtime_epoch"),
           py::arg("output_resources") = std::vector<ResourceId>{})
      .def_readonly("provider", &ResolvedProviderDomainScopeDTO::provider)
      .def_readonly("provider_runtime_epoch",
                    &ResolvedProviderDomainScopeDTO::provider_runtime_epoch)
      .def_property_readonly("output_resources",
                             [](const ResolvedProviderDomainScopeDTO& value) {
                               return immutable_snapshot(value.output_resources);
                             })
      .def(py::self == py::self);

  py::class_<ResolvedLifecycleControlDomainScopeDTO>(
      module, "ResolvedLifecycleControlDomainScopeDTO", py::is_final())
      .def(py::init<ControlDomainId, ControlDomainRevision, ResolvedProviderDomainScopeDTO>(),
           py::arg("control_domain_id"), py::arg("control_domain_revision"), py::arg("provider"))
      .def_property_readonly(
          "provider",
          [](const ResolvedLifecycleControlDomainScopeDTO& value) { return value.provider(); },
          py::return_value_policy::reference_internal)
      .def_readonly("control_domain_id", &ResolvedLifecycleControlDomainScopeDTO::control_domain_id)
      .def_readonly("control_domain_revision",
                    &ResolvedLifecycleControlDomainScopeDTO::control_domain_revision)
      .def("locally_valid", &ResolvedLifecycleControlDomainScopeDTO::locally_valid)
      .def(py::self == py::self)
      .def(py::self != py::self);

  py::class_<ResolvedLifecycleProviderOccurrenceDTO>(
      module, "ResolvedLifecycleProviderOccurrenceDTO", py::is_final())
      .def(py::init<ControlDomainId, ControlDomainRevision, ProviderInstanceId, ProviderManifestId,
                    ProviderManifestRevision, RuntimeUnitId, RuntimeEpoch, ContentDigest>(),
           py::arg("control_domain_id"), py::arg("control_domain_revision"),
           py::arg("provider_instance_id"), py::arg("provider_manifest_id"),
           py::arg("provider_manifest_revision"), py::arg("provider_runtime_unit_id"),
           py::arg("provider_runtime_epoch"), py::arg("lifecycle_plan_catalog_digest"))
      .def_readonly("control_domain_id", &ResolvedLifecycleProviderOccurrenceDTO::control_domain_id)
      .def_readonly("control_domain_revision",
                    &ResolvedLifecycleProviderOccurrenceDTO::control_domain_revision)
      .def_readonly("provider_instance_id",
                    &ResolvedLifecycleProviderOccurrenceDTO::provider_instance_id)
      .def_readonly("provider_manifest_id",
                    &ResolvedLifecycleProviderOccurrenceDTO::provider_manifest_id)
      .def_readonly("provider_manifest_revision",
                    &ResolvedLifecycleProviderOccurrenceDTO::provider_manifest_revision)
      .def_readonly("provider_runtime_unit_id",
                    &ResolvedLifecycleProviderOccurrenceDTO::provider_runtime_unit_id)
      .def_readonly("provider_runtime_epoch",
                    &ResolvedLifecycleProviderOccurrenceDTO::provider_runtime_epoch)
      .def_readonly("lifecycle_plan_catalog_digest",
                    &ResolvedLifecycleProviderOccurrenceDTO::lifecycle_plan_catalog_digest)
      .def("locally_valid", &ResolvedLifecycleProviderOccurrenceDTO::locally_valid)
      .def(py::self == py::self)
      .def(py::self != py::self);

  py::class_<ProviderConfigurationFenceOccurrenceDTO>(
      module, "ProviderConfigurationFenceOccurrenceDTO", py::is_final())
      .def(py::init<ControllableComponentId, ProviderInstanceId, ContentDigest,
                    std::vector<ResourceId>>(),
           py::arg("controllable_component_id"), py::arg("provider_instance_id"),
           py::arg("signal_configuration_digest"),
           py::arg("output_resources") = std::vector<ResourceId>{})
      .def_readonly("controllable_component_id",
                    &ProviderConfigurationFenceOccurrenceDTO::controllable_component_id)
      .def_readonly("provider_instance_id",
                    &ProviderConfigurationFenceOccurrenceDTO::provider_instance_id)
      .def_readonly("signal_configuration_digest",
                    &ProviderConfigurationFenceOccurrenceDTO::signal_configuration_digest)
      .def_property_readonly("output_resources",
                             [](const ProviderConfigurationFenceOccurrenceDTO& value) {
                               return immutable_snapshot(value.output_resources);
                             })
      .def("valid", &ProviderConfigurationFenceOccurrenceDTO::valid)
      .def(py::self == py::self);

  py::class_<ProviderConfigurationFenceDomainDTO>(module, "ProviderConfigurationFenceDomainDTO",
                                                  py::is_final())
      .def(py::init<std::optional<ProviderConfigurationFenceOccurrenceDTO>,
                    std::optional<ProviderConfigurationFenceOccurrenceDTO>>(),
           py::arg("predecessor_provider") = std::nullopt,
           py::arg("candidate_provider") = std::nullopt)
      .def_readonly("predecessor_provider",
                    &ProviderConfigurationFenceDomainDTO::predecessor_provider)
      .def_readonly("candidate_provider", &ProviderConfigurationFenceDomainDTO::candidate_provider)
      .def(py::self == py::self);

  py::class_<ConfigurationFenceDomainScopeDTO>(module, "ConfigurationFenceDomainScopeDTO",
                                               py::is_final())
      .def(py::init<ControlDomainId, std::optional<ControlDomainRevision>,
                    std::optional<ControlDomainRevision>, ProviderConfigurationFenceDomainDTO>(),
           py::arg("control_domain_id"), py::arg("predecessor_control_domain_revision"),
           py::arg("candidate_control_domain_revision"), py::arg("provider"))
      .def_property_readonly(
          "provider",
          [](const ConfigurationFenceDomainScopeDTO& value) {
            return std::get_if<ProviderConfigurationFenceDomainDTO>(&value.payload);
          },
          py::return_value_policy::reference_internal)
      .def(py::init<ControlDomainId, std::optional<ControlDomainRevision>,
                    std::optional<ControlDomainRevision>>(),
           py::arg("control_domain_id"),
           py::arg("predecessor_control_domain_revision") = std::nullopt,
           py::arg("candidate_control_domain_revision") = std::nullopt)
      .def_readonly("control_domain_id", &ConfigurationFenceDomainScopeDTO::control_domain_id)
      .def_readonly("predecessor_control_domain_revision",
                    &ConfigurationFenceDomainScopeDTO::predecessor_control_domain_revision)
      .def_readonly("candidate_control_domain_revision",
                    &ConfigurationFenceDomainScopeDTO::candidate_control_domain_revision)
      .def("valid", &ConfigurationFenceDomainScopeDTO::valid)
      .def(py::self == py::self)
      .def(py::self != py::self);

  py::class_<ConfigurationFenceBindingScopeDTO>(module, "ConfigurationFenceBindingScopeDTO",
                                                py::is_final())
      .def(py::init<ControlBindingId, std::optional<ControlBindingRevision>,
                    std::optional<ControlBindingRevision>>(),
           py::arg("control_binding_id"),
           py::arg("predecessor_control_binding_revision") = std::nullopt,
           py::arg("candidate_control_binding_revision") = std::nullopt)
      .def_readonly("control_binding_id", &ConfigurationFenceBindingScopeDTO::control_binding_id)
      .def_readonly("predecessor_control_binding_revision",
                    &ConfigurationFenceBindingScopeDTO::predecessor_control_binding_revision)
      .def_readonly("candidate_control_binding_revision",
                    &ConfigurationFenceBindingScopeDTO::candidate_control_binding_revision)
      .def("valid", &ConfigurationFenceBindingScopeDTO::valid)
      .def(py::self == py::self)
      .def(py::self != py::self);

  py::class_<ConfigurationFenceScopeDTO>(module, "ConfigurationFenceScopeDTO", py::is_final())
      .def(py::init<std::vector<ControllableComponentId>,
                    std::vector<ConfigurationFenceDomainScopeDTO>,
                    std::vector<ConfigurationFenceBindingScopeDTO>, std::vector<ResourceId>>(),
           py::arg("controllable_component_ids"), py::arg("domains"), py::arg("bindings"),
           py::arg("resources"))
      .def_property_readonly("controllable_component_ids",
                             [](const ConfigurationFenceScopeDTO& value) {
                               return immutable_snapshot(value.controllable_component_ids);
                             })
      .def_property_readonly(
          "domains",
          [](const ConfigurationFenceScopeDTO& value) { return immutable_snapshot(value.domains); })
      .def_property_readonly("bindings",
                             [](const ConfigurationFenceScopeDTO& value) {
                               return immutable_snapshot(value.bindings);
                             })
      .def_property_readonly("resources",
                             [](const ConfigurationFenceScopeDTO& value) {
                               return immutable_snapshot(value.resources);
                             })
      .def("valid", &ConfigurationFenceScopeDTO::valid)
      .def(py::self == py::self)
      .def(py::self != py::self);
}

}  // namespace manyforge::core::python
