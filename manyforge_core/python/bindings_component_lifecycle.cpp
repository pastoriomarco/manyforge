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
      .value("kRequestLedgerFull", RequestReplayStatusDTO::kRequestLedgerFull)
      .value("kStaleRequestOwnerEpoch", RequestReplayStatusDTO::kStaleRequestOwnerEpoch);

  py::enum_<ComponentLifecycleStateDTO>(module, "ComponentLifecycleStateDTO")
      .value("kUndeclared", ComponentLifecycleStateDTO::kUndeclared)
      .value("kDeclared", ComponentLifecycleStateDTO::kDeclared)
      .value("kPrepared", ComponentLifecycleStateDTO::kPrepared)
      .value("kActive", ComponentLifecycleStateDTO::kActive)
      .value("kDegraded", ComponentLifecycleStateDTO::kDegraded)
      .value("kFaulted", ComponentLifecycleStateDTO::kFaulted)
      .value("kDisabling", ComponentLifecycleStateDTO::kDisabling)
      .value("kDisabled", ComponentLifecycleStateDTO::kDisabled);

  py::enum_<ExpectedActiveControlConfigurationPresenceDTO>(
      module, "ExpectedActiveControlConfigurationPresenceDTO")
      .value("kAbsent", ExpectedActiveControlConfigurationPresenceDTO::kAbsent)
      .value("kPresent", ExpectedActiveControlConfigurationPresenceDTO::kPresent);

  py::enum_<LifecycleFactValueKindDTO>(module, "LifecycleFactValueKindDTO")
      .value("kUnknown", LifecycleFactValueKindDTO::kUnknown)
      .value("kBoolean", LifecycleFactValueKindDTO::kBoolean)
      .value("kInteger", LifecycleFactValueKindDTO::kInteger)
      .value("kToken", LifecycleFactValueKindDTO::kToken);

  py::enum_<ConfigurationFenceState>(module, "ConfigurationFenceState")
      .value("kHeld", ConfigurationFenceState::kHeld)
      .value("kReleasePending", ConfigurationFenceState::kReleasePending)
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
      .def(py::init<RequestId, RequestOwnerEpoch, std::string, std::optional<std::string>>(),
           py::arg("request_id"), py::arg("request_owner_epoch"), py::arg("source_label"),
           py::arg("correlation_id") = std::nullopt)
      .def_readonly("request_id", &PublicRequestContextDTO::request_id)
      .def_readonly("request_owner_epoch", &PublicRequestContextDTO::request_owner_epoch)
      .def_readonly("source_label", &PublicRequestContextDTO::source_label)
      .def_readonly("correlation_id", &PublicRequestContextDTO::correlation_id)
      .def(py::self == py::self)
      .def(py::self != py::self);

  py::class_<ExpectedActiveControlConfigurationDTO>(module, "ExpectedActiveControlConfigurationDTO",
                                                    py::is_final())
      .def(py::init<ExpectedActiveControlConfigurationPresenceDTO,
                    std::optional<ActiveControlConfigurationId>,
                    std::optional<ActiveControlConfigurationGeneration>>(),
           py::arg("presence"), py::arg("active_control_configuration_id") = std::nullopt,
           py::arg("active_control_configuration_generation") = std::nullopt)
      .def_readonly("presence", &ExpectedActiveControlConfigurationDTO::presence)
      .def_readonly("active_control_configuration_id",
                    &ExpectedActiveControlConfigurationDTO::active_control_configuration_id)
      .def_readonly("active_control_configuration_generation",
                    &ExpectedActiveControlConfigurationDTO::active_control_configuration_generation)
      .def("locally_valid", &ExpectedActiveControlConfigurationDTO::locally_valid)
      .def(py::self == py::self)
      .def(py::self != py::self);

  py::class_<ExpectedControlBindingRevisionDTO>(module, "ExpectedControlBindingRevisionDTO",
                                                py::is_final())
      .def(py::init<ControlBindingId, ControlBindingRevision, ControlRouteId>(),
           py::arg("control_binding_id"), py::arg("control_binding_revision"),
           py::arg("control_route_id"))
      .def_readonly("control_binding_id", &ExpectedControlBindingRevisionDTO::control_binding_id)
      .def_readonly("control_binding_revision",
                    &ExpectedControlBindingRevisionDTO::control_binding_revision)
      .def_readonly("control_route_id", &ExpectedControlBindingRevisionDTO::control_route_id)
      .def("locally_valid", &ExpectedControlBindingRevisionDTO::locally_valid)
      .def(py::self == py::self)
      .def(py::self != py::self);

  py::class_<ProgramNodeAdmissionEvidenceDTO>(module, "ProgramNodeAdmissionEvidenceDTO",
                                              py::is_final())
      .def(py::init<RequestOwnerEpoch, ProgramRunId, CellAdmissionClaimGeneration, std::string,
                    std::string, std::string>(),
           py::arg("program_runner_owner_epoch"), py::arg("program_run_id"),
           py::arg("parent_program_cycle_claim_generation"), py::arg("node_instance_id"),
           py::arg("node_attempt_id"), py::arg("owner_api"))
      .def_readonly("program_runner_owner_epoch",
                    &ProgramNodeAdmissionEvidenceDTO::program_runner_owner_epoch)
      .def_readonly("program_run_id", &ProgramNodeAdmissionEvidenceDTO::program_run_id)
      .def_readonly("parent_program_cycle_claim_generation",
                    &ProgramNodeAdmissionEvidenceDTO::parent_program_cycle_claim_generation)
      .def_readonly("node_instance_id", &ProgramNodeAdmissionEvidenceDTO::node_instance_id)
      .def_readonly("node_attempt_id", &ProgramNodeAdmissionEvidenceDTO::node_attempt_id)
      .def_readonly("owner_api", &ProgramNodeAdmissionEvidenceDTO::owner_api)
      .def("locally_valid", &ProgramNodeAdmissionEvidenceDTO::locally_valid)
      .def(py::self == py::self);

  py::class_<LifecycleFactValueDTO>(module, "LifecycleFactValueDTO", py::is_final())
      .def(py::init<LifecycleFactValueKindDTO, std::optional<bool>, std::optional<std::int64_t>,
                    std::optional<std::string>>(),
           py::arg("kind"), py::arg("boolean_value") = std::nullopt,
           py::arg("integer_value") = std::nullopt, py::arg("token_value") = std::nullopt)
      .def_readonly("kind", &LifecycleFactValueDTO::kind)
      .def_readonly("boolean_value", &LifecycleFactValueDTO::boolean_value)
      .def_readonly("integer_value", &LifecycleFactValueDTO::integer_value)
      .def_readonly("token_value", &LifecycleFactValueDTO::token_value)
      .def("locally_valid", &LifecycleFactValueDTO::locally_valid)
      .def(py::self == py::self)
      .def(py::self != py::self);

  py::class_<LifecycleFactDTO>(module, "LifecycleFactDTO", py::is_final())
      .def(py::init<LifecycleFactId, LifecycleFactValueDTO>(), py::arg("lifecycle_fact_id"),
           py::arg("value"))
      .def_readonly("lifecycle_fact_id", &LifecycleFactDTO::lifecycle_fact_id)
      .def_readonly("value", &LifecycleFactDTO::value)
      .def("locally_valid", &LifecycleFactDTO::locally_valid)
      .def(py::self == py::self)
      .def(py::self != py::self);

  py::class_<BrandLifecycleObservationDTO>(module, "BrandLifecycleObservationDTO", py::is_final())
      .def(py::init<LifecycleTransactionId, LifecycleTransactionGeneration, ControllableComponentId,
                    ControlDomainId, ControlDomainRevision, ProviderInstanceId, ProviderManifestId,
                    ProviderManifestRevision, RuntimeUnitId, RuntimeEpoch, std::uint64_t,
                    std::uint64_t, std::int64_t, std::int64_t, std::int64_t,
                    ComponentLifecycleStateDTO, std::vector<LifecycleFactDTO>>(),
           py::arg("lifecycle_transaction_id"), py::arg("lifecycle_transaction_generation"),
           py::arg("controllable_component_id"), py::arg("control_domain_id"),
           py::arg("control_domain_revision"), py::arg("provider_instance_id"),
           py::arg("provider_manifest_id"), py::arg("provider_manifest_revision"),
           py::arg("provider_runtime_unit_id"), py::arg("provider_runtime_epoch"),
           py::arg("source_sequence"), py::arg("observation_generation"),
           py::arg("source_timestamp_ns"), py::arg("received_timestamp_ns"),
           py::arg("fresh_until_ns"), py::arg("observed_state"), py::arg("facts"))
      .def_readonly("lifecycle_transaction_id",
                    &BrandLifecycleObservationDTO::lifecycle_transaction_id)
      .def_readonly("lifecycle_transaction_generation",
                    &BrandLifecycleObservationDTO::lifecycle_transaction_generation)
      .def_readonly("controllable_component_id",
                    &BrandLifecycleObservationDTO::controllable_component_id)
      .def_readonly("control_domain_id", &BrandLifecycleObservationDTO::control_domain_id)
      .def_readonly("control_domain_revision",
                    &BrandLifecycleObservationDTO::control_domain_revision)
      .def_readonly("provider_instance_id", &BrandLifecycleObservationDTO::provider_instance_id)
      .def_readonly("provider_manifest_id", &BrandLifecycleObservationDTO::provider_manifest_id)
      .def_readonly("provider_manifest_revision",
                    &BrandLifecycleObservationDTO::provider_manifest_revision)
      .def_readonly("provider_runtime_unit_id",
                    &BrandLifecycleObservationDTO::provider_runtime_unit_id)
      .def_readonly("provider_runtime_epoch", &BrandLifecycleObservationDTO::provider_runtime_epoch)
      .def_readonly("source_sequence", &BrandLifecycleObservationDTO::source_sequence)
      .def_readonly("observation_generation", &BrandLifecycleObservationDTO::observation_generation)
      .def_readonly("source_timestamp_ns", &BrandLifecycleObservationDTO::source_timestamp_ns)
      .def_readonly("received_timestamp_ns", &BrandLifecycleObservationDTO::received_timestamp_ns)
      .def_readonly("fresh_until_ns", &BrandLifecycleObservationDTO::fresh_until_ns)
      .def_readonly("observed_state", &BrandLifecycleObservationDTO::observed_state)
      .def_property_readonly(
          "facts",
          [](const BrandLifecycleObservationDTO& value) { return immutable_snapshot(value.facts); })
      .def("locally_valid", &BrandLifecycleObservationDTO::locally_valid)
      .def(py::self == py::self)
      .def(py::self != py::self);

  py::class_<ProviderDomainActiveConfigurationDTO>(module, "ProviderDomainActiveConfigurationDTO",
                                                   py::is_final())
      .def(py::init<ActiveControlConfigurationId, ActiveControlConfigurationGeneration,
                    RuntimeUnitId, RuntimeEpoch, std::uint64_t, ContentDigest>(),
           py::arg("active_control_configuration_id"),
           py::arg("active_control_configuration_generation"), py::arg("source_runtime_unit_id"),
           py::arg("source_runtime_epoch"), py::arg("source_sequence"),
           py::arg("signal_configuration_digest"))
      .def_readonly("active_control_configuration_id",
                    &ProviderDomainActiveConfigurationDTO::active_control_configuration_id)
      .def_readonly("active_control_configuration_generation",
                    &ProviderDomainActiveConfigurationDTO::active_control_configuration_generation)
      .def_readonly("source_runtime_unit_id",
                    &ProviderDomainActiveConfigurationDTO::source_runtime_unit_id)
      .def_readonly("source_runtime_epoch",
                    &ProviderDomainActiveConfigurationDTO::source_runtime_epoch)
      .def_readonly("source_sequence", &ProviderDomainActiveConfigurationDTO::source_sequence)
      .def_readonly("signal_configuration_digest",
                    &ProviderDomainActiveConfigurationDTO::signal_configuration_digest)
      .def("locally_valid", &ProviderDomainActiveConfigurationDTO::locally_valid)
      .def(py::self == py::self);

  py::class_<ResolvedProviderDomainScopeDTO>(module, "ResolvedProviderDomainScopeDTO",
                                             py::is_final())
      .def(py::init<ProviderControlDomainDTO, RuntimeEpoch,
                    std::optional<ProviderDomainActiveConfigurationDTO>, std::vector<ResourceId>>(),
           py::arg("provider"), py::arg("provider_runtime_epoch"),
           py::arg("active_control_configuration") = std::nullopt,
           py::arg("output_resources") = std::vector<ResourceId>{})
      .def_readonly("provider", &ResolvedProviderDomainScopeDTO::provider)
      .def_readonly("provider_runtime_epoch",
                    &ResolvedProviderDomainScopeDTO::provider_runtime_epoch)
      .def_readonly("active_control_configuration",
                    &ResolvedProviderDomainScopeDTO::active_control_configuration)
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
      .def(py::init<ControlDomainId, ControlDomainRevision, ControllerTopologyId, TopologyRevision,
                    std::optional<ActiveControlConfigurationDTO>,
                    std::vector<ExpectedControlBindingRevisionDTO>, std::vector<ResourceId>>(),
           py::arg("control_domain_id"), py::arg("control_domain_revision"),
           py::arg("controller_topology_id"), py::arg("topology_revision"),
           py::arg("active_control_configuration"), py::arg("current_control_bindings"),
           py::arg("current_resources"))
      .def_readonly("control_domain_id", &ResolvedLifecycleControlDomainScopeDTO::control_domain_id)
      .def_readonly("control_domain_revision",
                    &ResolvedLifecycleControlDomainScopeDTO::control_domain_revision)
      .def_property_readonly(
          "controller_topology_id",
          [](const ResolvedLifecycleControlDomainScopeDTO& value) -> const ControllerTopologyId& {
            return value.controller().controller_topology_id;
          },
          py::return_value_policy::reference_internal)
      .def_property_readonly(
          "topology_revision",
          [](const ResolvedLifecycleControlDomainScopeDTO& value) -> const TopologyRevision& {
            return value.controller().topology_revision;
          },
          py::return_value_policy::reference_internal)
      .def_property_readonly(
          "active_control_configuration",
          [](const ResolvedLifecycleControlDomainScopeDTO& value)
              -> const std::optional<ActiveControlConfigurationDTO>& {
            return value.controller().active_control_configuration;
          },
          py::return_value_policy::reference_internal)
      .def_property_readonly(
          "current_control_bindings",
          [](const ResolvedLifecycleControlDomainScopeDTO& value) {
            return immutable_snapshot(value.controller().current_control_bindings);
          })
      .def_property_readonly("current_resources",
                             [](const ResolvedLifecycleControlDomainScopeDTO& value) {
                               return immutable_snapshot(value.controller().current_resources);
                             })
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

  py::class_<ComponentLifecycleCurrentSnapshotDTO>(module, "ComponentLifecycleCurrentSnapshotDTO",
                                                   py::is_final())
      .def(py::init<ContentDigest, ControllableComponentId, ComponentLifecycleGeneration,
                    ComponentLifecycleStateDTO, ActiveBundlePublicationReceiptDTO,
                    std::vector<ResolvedLifecycleControlDomainScopeDTO>,
                    std::vector<ResolvedLifecycleProviderOccurrenceDTO>>(),
           py::arg("snapshot_digest"), py::arg("controllable_component_id"),
           py::arg("component_lifecycle_generation"), py::arg("component_lifecycle_state"),
           py::arg("active_publication_receipt"), py::arg("domains"),
           py::arg("provider_occurrences"))
      .def_readonly("snapshot_digest", &ComponentLifecycleCurrentSnapshotDTO::snapshot_digest)
      .def_readonly("controllable_component_id",
                    &ComponentLifecycleCurrentSnapshotDTO::controllable_component_id)
      .def_readonly("component_lifecycle_generation",
                    &ComponentLifecycleCurrentSnapshotDTO::component_lifecycle_generation)
      .def_readonly("component_lifecycle_state",
                    &ComponentLifecycleCurrentSnapshotDTO::component_lifecycle_state)
      .def_readonly("active_publication_receipt",
                    &ComponentLifecycleCurrentSnapshotDTO::active_publication_receipt)
      .def_property_readonly("domains",
                             [](const ComponentLifecycleCurrentSnapshotDTO& value) {
                               return immutable_snapshot(value.domains);
                             })
      .def_property_readonly("provider_occurrences",
                             [](const ComponentLifecycleCurrentSnapshotDTO& value) {
                               return immutable_snapshot(value.provider_occurrences);
                             })
      .def("locally_valid", &ComponentLifecycleCurrentSnapshotDTO::locally_valid)
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

  py::class_<StopCertificateRefDTO>(module, "StopCertificateRefDTO", py::is_final())
      .def(py::init<StopCertificateId, StopCertificateRevision, StopId, StopGeneration>(),
           py::arg("stop_certificate_id"), py::arg("stop_certificate_revision"), py::arg("stop_id"),
           py::arg("stop_generation"))
      .def_readonly("stop_certificate_id", &StopCertificateRefDTO::stop_certificate_id)
      .def_readonly("stop_certificate_revision", &StopCertificateRefDTO::stop_certificate_revision)
      .def_readonly("stop_id", &StopCertificateRefDTO::stop_id)
      .def_readonly("stop_generation", &StopCertificateRefDTO::stop_generation)
      .def("locally_valid", &StopCertificateRefDTO::locally_valid)
      .def(py::self == py::self)
      .def(py::self != py::self);
}

}  // namespace manyforge::core::python
