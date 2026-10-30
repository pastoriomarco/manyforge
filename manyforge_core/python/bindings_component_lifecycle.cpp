#include <pybind11/operators.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <manyforge_core/component_lifecycle_plan_catalog_codec.hpp>
#include <manyforge_core/ports/icclc_journal.hpp>
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

  py::enum_<ComponentLifecycleIntentKindDTO>(module, "ComponentLifecycleIntentKindDTO")
      .value("kPrepare", ComponentLifecycleIntentKindDTO::kPrepare)
      .value("kEnable", ComponentLifecycleIntentKindDTO::kEnable)
      .value("kDisable", ComponentLifecycleIntentKindDTO::kDisable)
      .value("kRecover", ComponentLifecycleIntentKindDTO::kRecover);

  py::enum_<ComponentLifecycleStateDTO>(module, "ComponentLifecycleStateDTO")
      .value("kUndeclared", ComponentLifecycleStateDTO::kUndeclared)
      .value("kDeclared", ComponentLifecycleStateDTO::kDeclared)
      .value("kPrepared", ComponentLifecycleStateDTO::kPrepared)
      .value("kActive", ComponentLifecycleStateDTO::kActive)
      .value("kDegraded", ComponentLifecycleStateDTO::kDegraded)
      .value("kFaulted", ComponentLifecycleStateDTO::kFaulted)
      .value("kDisabling", ComponentLifecycleStateDTO::kDisabling)
      .value("kDisabled", ComponentLifecycleStateDTO::kDisabled);

  py::enum_<ComponentLifecycleRequestedStatePresenceDTO>(
      module, "ComponentLifecycleRequestedStatePresenceDTO")
      .value("kUnresolved", ComponentLifecycleRequestedStatePresenceDTO::kUnresolved)
      .value("kResolved", ComponentLifecycleRequestedStatePresenceDTO::kResolved);

  py::enum_<ExpectedActiveControlConfigurationPresenceDTO>(
      module, "ExpectedActiveControlConfigurationPresenceDTO")
      .value("kAbsent", ExpectedActiveControlConfigurationPresenceDTO::kAbsent)
      .value("kPresent", ExpectedActiveControlConfigurationPresenceDTO::kPresent);

  py::enum_<LifecycleFactValueKindDTO>(module, "LifecycleFactValueKindDTO")
      .value("kUnknown", LifecycleFactValueKindDTO::kUnknown)
      .value("kBoolean", LifecycleFactValueKindDTO::kBoolean)
      .value("kInteger", LifecycleFactValueKindDTO::kInteger)
      .value("kToken", LifecycleFactValueKindDTO::kToken);

  py::enum_<LifecycleTransitionOutcomeDTO>(module, "LifecycleTransitionOutcomeDTO")
      .value("kSucceeded", LifecycleTransitionOutcomeDTO::kSucceeded)
      .value("kFailed", LifecycleTransitionOutcomeDTO::kFailed)
      .value("kConnectionLost", LifecycleTransitionOutcomeDTO::kConnectionLost);

  py::enum_<LifecyclePostconditionResultDTO>(module, "LifecyclePostconditionResultDTO")
      .value("kSatisfied", LifecyclePostconditionResultDTO::kSatisfied)
      .value("kNotSatisfied", LifecyclePostconditionResultDTO::kNotSatisfied)
      .value("kInconclusive", LifecyclePostconditionResultDTO::kInconclusive);

  py::enum_<ComponentLifecycleDomainActionStateDTO>(module,
                                                    "ComponentLifecycleDomainActionStateDTO")
      .value("kReserved", ComponentLifecycleDomainActionStateDTO::kReserved)
      .value("kNotStarted", ComponentLifecycleDomainActionStateDTO::kNotStarted)
      .value("kExecuting", ComponentLifecycleDomainActionStateDTO::kExecuting)
      .value("kVerifying", ComponentLifecycleDomainActionStateDTO::kVerifying)
      .value("kSucceeded", ComponentLifecycleDomainActionStateDTO::kSucceeded)
      .value("kFailed", ComponentLifecycleDomainActionStateDTO::kFailed);

  py::enum_<ComponentLifecycleTransactionPhaseDTO>(module, "ComponentLifecycleTransactionPhaseDTO")
      .value("kAccepted", ComponentLifecycleTransactionPhaseDTO::kAccepted)
      .value("kAcquiringDomains", ComponentLifecycleTransactionPhaseDTO::kAcquiringDomains)
      .value("kContaining", ComponentLifecycleTransactionPhaseDTO::kContaining)
      .value("kExecuting", ComponentLifecycleTransactionPhaseDTO::kExecuting)
      .value("kVerifying", ComponentLifecycleTransactionPhaseDTO::kVerifying)
      .value("kOutcomeUnknown", ComponentLifecycleTransactionPhaseDTO::kOutcomeUnknown)
      .value("kSucceeded", ComponentLifecycleTransactionPhaseDTO::kSucceeded)
      .value("kFailed", ComponentLifecycleTransactionPhaseDTO::kFailed);

  py::enum_<ComponentLifecycleTerminalResultDTO>(module, "ComponentLifecycleTerminalResultDTO")
      .value("kSucceeded", ComponentLifecycleTerminalResultDTO::kSucceeded)
      .value("kFailed", ComponentLifecycleTerminalResultDTO::kFailed)
      .value("kConnectionLost", ComponentLifecycleTerminalResultDTO::kConnectionLost);

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

  py::class_<ComponentLifecycleRequestedStateDTO>(module, "ComponentLifecycleRequestedStateDTO",
                                                  py::is_final())
      .def(py::init<ComponentLifecycleRequestedStatePresenceDTO,
                    std::optional<ComponentLifecycleStateDTO>>(),
           py::arg("presence"), py::arg("state") = std::nullopt)
      .def_readonly("presence", &ComponentLifecycleRequestedStateDTO::presence)
      .def_readonly("state", &ComponentLifecycleRequestedStateDTO::state)
      .def("locally_valid", &ComponentLifecycleRequestedStateDTO::locally_valid)
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

  py::class_<ExpectedProviderDomainScopeDTO>(module, "ExpectedProviderDomainScopeDTO",
                                             py::is_final())
      .def(py::init<ProviderControlDomainDTO, ExpectedActiveControlConfigurationDTO>(),
           py::arg("provider"), py::arg("expected_active_control_configuration"))
      .def_readonly("provider", &ExpectedProviderDomainScopeDTO::provider)
      .def_readonly("expected_active_control_configuration",
                    &ExpectedProviderDomainScopeDTO::expected_active_control_configuration)
      .def(py::self == py::self);

  py::class_<ExpectedControlDomainRevisionDTO>(module, "ExpectedControlDomainRevisionDTO",
                                               py::is_final())
      .def(py::init<ControlDomainId, ControlDomainRevision, ExpectedProviderDomainScopeDTO>(),
           py::arg("control_domain_id"), py::arg("control_domain_revision"), py::arg("provider"))
      .def_property_readonly(
          "provider",
          [](const ExpectedControlDomainRevisionDTO& value) { return value.provider(); },
          py::return_value_policy::reference_internal)
      .def(py::init<ControlDomainId, ControlDomainRevision, ControllerTopologyId, TopologyRevision,
                    ExpectedActiveControlConfigurationDTO,
                    std::vector<ExpectedControlBindingRevisionDTO>>(),
           py::arg("control_domain_id"), py::arg("control_domain_revision"),
           py::arg("controller_topology_id"), py::arg("topology_revision"),
           py::arg("expected_active_control_configuration"), py::arg("expected_control_bindings"))
      .def_readonly("control_domain_id", &ExpectedControlDomainRevisionDTO::control_domain_id)
      .def_readonly("control_domain_revision",
                    &ExpectedControlDomainRevisionDTO::control_domain_revision)
      .def_property_readonly(
          "controller_topology_id",
          [](const ExpectedControlDomainRevisionDTO& value) -> const ControllerTopologyId& {
            return value.controller().controller_topology_id;
          },
          py::return_value_policy::reference_internal)
      .def_property_readonly(
          "topology_revision",
          [](const ExpectedControlDomainRevisionDTO& value) -> const TopologyRevision& {
            return value.controller().topology_revision;
          },
          py::return_value_policy::reference_internal)
      .def_property_readonly(
          "expected_active_control_configuration",
          [](const ExpectedControlDomainRevisionDTO& value)
              -> const ExpectedActiveControlConfigurationDTO& {
            return value.controller().expected_active_control_configuration;
          },
          py::return_value_policy::reference_internal)
      .def_property_readonly(
          "expected_control_bindings",
          [](const ExpectedControlDomainRevisionDTO& value) {
            return immutable_snapshot(value.controller().expected_control_bindings);
          })
      .def("locally_valid", &ExpectedControlDomainRevisionDTO::locally_valid)
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

  py::class_<ComponentLifecycleIntentDTO>(module, "ComponentLifecycleIntentDTO", py::is_final())
      .def(py::init<PublicRequestContextDTO, ControllableComponentId,
                    ComponentLifecycleIntentKindDTO, GraphRevision, ComponentLifecycleGeneration,
                    std::vector<ExpectedControlDomainRevisionDTO>, std::optional<std::string>,
                    RequestAttributionDTO, AdmissionGeneration,
                    std::optional<ProgramNodeAdmissionEvidenceDTO>>(),
           py::arg("request_context"), py::arg("controllable_component_id"), py::arg("intent"),
           py::arg("expected_graph_revision"), py::arg("expected_component_lifecycle_generation"),
           py::arg("expected_control_domains"), py::arg("reason") = std::nullopt,
           py::arg("attribution") = RequestAttributionDTO{},
           py::arg("expected_admission_generation") = AdmissionGeneration(1U),
           py::arg("program_node_evidence") = std::nullopt)
      .def_readonly("request_context", &ComponentLifecycleIntentDTO::request_context)
      .def_readonly("controllable_component_id",
                    &ComponentLifecycleIntentDTO::controllable_component_id)
      .def_readonly("intent", &ComponentLifecycleIntentDTO::intent)
      .def_readonly("expected_graph_revision",
                    &ComponentLifecycleIntentDTO::expected_graph_revision)
      .def_readonly("expected_component_lifecycle_generation",
                    &ComponentLifecycleIntentDTO::expected_component_lifecycle_generation)
      .def_property_readonly("expected_control_domains",
                             [](const ComponentLifecycleIntentDTO& value) {
                               return immutable_snapshot(value.expected_control_domains);
                             })
      .def_readonly("reason", &ComponentLifecycleIntentDTO::reason)
      .def_readonly("attribution", &ComponentLifecycleIntentDTO::attribution)
      .def_readonly("expected_admission_generation",
                    &ComponentLifecycleIntentDTO::expected_admission_generation)
      .def_readonly("program_node_evidence", &ComponentLifecycleIntentDTO::program_node_evidence)
      .def("locally_valid", &ComponentLifecycleIntentDTO::locally_valid)
      .def("replay_equal", &ComponentLifecycleIntentDTO::replay_equal)
      .def(py::self == py::self)
      .def(py::self != py::self);

  py::class_<ComponentLifecycleAcceptedDTO>(module, "ComponentLifecycleAcceptedDTO", py::is_final())
      .def(py::init<RequestId, RequestReplayStatusDTO, LifecycleTransactionId,
                    LifecycleTransactionGeneration, ComponentLifecycleGeneration, std::int64_t,
                    std::int64_t>(),
           py::arg("request_id"), py::arg("replay_status"), py::arg("lifecycle_transaction_id"),
           py::arg("lifecycle_transaction_generation"), py::arg("component_lifecycle_generation"),
           py::arg("accepted_at_ns"), py::arg("timeout_duration_ns"))
      .def_readonly("request_id", &ComponentLifecycleAcceptedDTO::request_id)
      .def_readonly("replay_status", &ComponentLifecycleAcceptedDTO::replay_status)
      .def_readonly("lifecycle_transaction_id",
                    &ComponentLifecycleAcceptedDTO::lifecycle_transaction_id)
      .def_readonly("lifecycle_transaction_generation",
                    &ComponentLifecycleAcceptedDTO::lifecycle_transaction_generation)
      .def_readonly("component_lifecycle_generation",
                    &ComponentLifecycleAcceptedDTO::component_lifecycle_generation)
      .def_readonly("accepted_at_ns", &ComponentLifecycleAcceptedDTO::accepted_at_ns)
      .def_readonly("timeout_duration_ns", &ComponentLifecycleAcceptedDTO::timeout_duration_ns)
      .def("locally_valid", &ComponentLifecycleAcceptedDTO::locally_valid)
      .def(py::self == py::self)
      .def(py::self != py::self);

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

  py::class_<LifecycleFactPredicateDTO>(module, "LifecycleFactPredicateDTO", py::is_final())
      .def(py::init<LifecycleFactId, LifecycleFactValueDTO>(), py::arg("lifecycle_fact_id"),
           py::arg("expected_value"))
      .def_readonly("lifecycle_fact_id", &LifecycleFactPredicateDTO::lifecycle_fact_id)
      .def_readonly("expected_value", &LifecycleFactPredicateDTO::expected_value)
      .def("locally_valid", &LifecycleFactPredicateDTO::locally_valid)
      .def(py::self == py::self)
      .def(py::self != py::self);

  py::class_<LifecyclePredicateDTO>(module, "LifecyclePredicateDTO", py::is_final())
      .def(py::init<std::vector<ComponentLifecycleStateDTO>,
                    std::vector<LifecycleFactPredicateDTO>>(),
           py::arg("allowed_states"), py::arg("fact_predicates"))
      .def_property_readonly("allowed_states",
                             [](const LifecyclePredicateDTO& value) {
                               return immutable_snapshot(value.allowed_states);
                             })
      .def_property_readonly("fact_predicates",
                             [](const LifecyclePredicateDTO& value) {
                               return immutable_snapshot(value.fact_predicates);
                             })
      .def("locally_valid", &LifecyclePredicateDTO::locally_valid)
      .def(py::self == py::self)
      .def(py::self != py::self);

  py::class_<LifecyclePrimitiveTransitionDTO>(module, "LifecyclePrimitiveTransitionDTO",
                                              py::is_final())
      .def(py::init<LifecyclePrimitiveId, LifecyclePredicateDTO, LifecyclePredicateDTO>(),
           py::arg("lifecycle_primitive_id"), py::arg("eligibility_predicate"),
           py::arg("postcondition_predicate"))
      .def_readonly("lifecycle_primitive_id",
                    &LifecyclePrimitiveTransitionDTO::lifecycle_primitive_id)
      .def_readonly("eligibility_predicate",
                    &LifecyclePrimitiveTransitionDTO::eligibility_predicate)
      .def_readonly("postcondition_predicate",
                    &LifecyclePrimitiveTransitionDTO::postcondition_predicate)
      .def("locally_valid", &LifecyclePrimitiveTransitionDTO::locally_valid)
      .def(py::self == py::self)
      .def(py::self != py::self);

  py::class_<DeviceConfiguredLifecycleLimitDTO>(module, "DeviceConfiguredLifecycleLimitDTO",
                                                py::is_final())
      .def(py::init<>())
      .def(py::self == py::self)
      .def(py::self != py::self);

  py::class_<LifecycleIntentPlanDTO>(module, "LifecycleIntentPlanDTO", py::is_final())
      .def(py::init<ComponentLifecycleIntentKindDTO, ComponentLifecycleStateDTO,
                    std::optional<LifecyclePredicateDTO>, LifecyclePredicateDTO, std::int64_t,
                    std::vector<LifecyclePrimitiveTransitionDTO>>(),
           py::arg("intent"), py::arg("requested_state"), py::arg("recover_target_eligibility"),
           py::arg("requested_postcondition"), py::arg("timeout_duration_ns"),
           py::arg("transitions"))
      .def(py::init<ComponentLifecycleIntentKindDTO, ComponentLifecycleStateDTO,
                    std::optional<LifecyclePredicateDTO>, LifecyclePredicateDTO,
                    LifecyclePlanTimeoutPolicyDTO, std::vector<LifecyclePrimitiveTransitionDTO>>(),
           py::arg("intent"), py::arg("requested_state"), py::arg("recover_target_eligibility"),
           py::arg("requested_postcondition"), py::arg("timeout_policy"), py::arg("transitions"))
      .def_readonly("intent", &LifecycleIntentPlanDTO::intent)
      .def_readonly("requested_state", &LifecycleIntentPlanDTO::requested_state)
      .def_readonly("recover_target_eligibility",
                    &LifecycleIntentPlanDTO::recover_target_eligibility)
      .def_readonly("requested_postcondition", &LifecycleIntentPlanDTO::requested_postcondition)
      .def_property_readonly("timeout_duration_ns",
                             [](const LifecycleIntentPlanDTO& value) {
                               return std::get<std::int64_t>(value.timeout_policy);
                             })
      .def_readonly("timeout_policy", &LifecycleIntentPlanDTO::timeout_policy)
      .def_property_readonly(
          "transitions",
          [](const LifecycleIntentPlanDTO& value) { return immutable_snapshot(value.transitions); })
      .def("locally_valid", &LifecycleIntentPlanDTO::locally_valid)
      .def(py::self == py::self)
      .def(py::self != py::self);

  py::class_<BrandLifecyclePlanCatalogDTO>(module, "BrandLifecyclePlanCatalogDTO", py::is_final())
      .def(py::init<ProviderManifestId, ProviderManifestRevision, ContentDigest,
                    std::vector<LifecycleIntentPlanDTO>>(),
           py::arg("provider_manifest_id"), py::arg("provider_manifest_revision"),
           py::arg("catalog_digest"), py::arg("plans"))
      .def_readonly("provider_manifest_id", &BrandLifecyclePlanCatalogDTO::provider_manifest_id)
      .def_readonly("provider_manifest_revision",
                    &BrandLifecyclePlanCatalogDTO::provider_manifest_revision)
      .def_readonly("catalog_digest", &BrandLifecyclePlanCatalogDTO::catalog_digest)
      .def_property_readonly(
          "plans",
          [](const BrandLifecyclePlanCatalogDTO& value) { return immutable_snapshot(value.plans); })
      .def("locally_valid", &BrandLifecyclePlanCatalogDTO::locally_valid)
      .def(py::self == py::self)
      .def(py::self != py::self);

  module.def(
      "make_brand_lifecycle_plan_catalog",
      [](ProviderManifestId provider_manifest_id,
         ProviderManifestRevision provider_manifest_revision,
         std::vector<LifecycleIntentPlanDTO> plans) {
        auto result =
            MakeBrandLifecyclePlanCatalog(std::move(provider_manifest_id),
                                          std::move(provider_manifest_revision), std::move(plans));
        if (!result.ok()) {
          throw py::value_error(result.error().message);
        }
        return std::move(result.value());
      },
      py::arg("provider_manifest_id"), py::arg("provider_manifest_revision"), py::arg("plans"));
  module.def(
      "encode_brand_lifecycle_plan_catalog",
      [](const BrandLifecyclePlanCatalogDTO& catalog) {
        auto result = EncodeBrandLifecyclePlanCatalog(catalog);
        if (!result.ok()) {
          throw py::value_error(result.error().message);
        }
        return std::move(result.value());
      },
      py::arg("catalog"));

  py::class_<BrandLifecycleObservationQueryDTO>(module, "BrandLifecycleObservationQueryDTO",
                                                py::is_final())
      .def(py::init<LifecycleTransactionId, LifecycleTransactionGeneration, ControllableComponentId,
                    ControlDomainId, ControlDomainRevision, ProviderInstanceId, ProviderManifestId,
                    ProviderManifestRevision, RuntimeUnitId, RuntimeEpoch>(),
           py::arg("lifecycle_transaction_id"), py::arg("lifecycle_transaction_generation"),
           py::arg("controllable_component_id"), py::arg("control_domain_id"),
           py::arg("control_domain_revision"), py::arg("provider_instance_id"),
           py::arg("provider_manifest_id"), py::arg("provider_manifest_revision"),
           py::arg("provider_runtime_unit_id"), py::arg("provider_runtime_epoch"))
      .def_readonly("lifecycle_transaction_id",
                    &BrandLifecycleObservationQueryDTO::lifecycle_transaction_id)
      .def_readonly("lifecycle_transaction_generation",
                    &BrandLifecycleObservationQueryDTO::lifecycle_transaction_generation)
      .def_readonly("controllable_component_id",
                    &BrandLifecycleObservationQueryDTO::controllable_component_id)
      .def_readonly("control_domain_id", &BrandLifecycleObservationQueryDTO::control_domain_id)
      .def_readonly("control_domain_revision",
                    &BrandLifecycleObservationQueryDTO::control_domain_revision)
      .def_readonly("provider_instance_id",
                    &BrandLifecycleObservationQueryDTO::provider_instance_id)
      .def_readonly("provider_manifest_id",
                    &BrandLifecycleObservationQueryDTO::provider_manifest_id)
      .def_readonly("provider_manifest_revision",
                    &BrandLifecycleObservationQueryDTO::provider_manifest_revision)
      .def_readonly("provider_runtime_unit_id",
                    &BrandLifecycleObservationQueryDTO::provider_runtime_unit_id)
      .def_readonly("provider_runtime_epoch",
                    &BrandLifecycleObservationQueryDTO::provider_runtime_epoch)
      .def("locally_valid", &BrandLifecycleObservationQueryDTO::locally_valid)
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
      .def("matches", &BrandLifecycleObservationDTO::matches)
      .def(py::self == py::self)
      .def(py::self != py::self);

  py::class_<BrandLifecyclePrimitiveIntentDTO>(module, "BrandLifecyclePrimitiveIntentDTO",
                                               py::is_final())
      .def(py::init<LifecycleTransactionId, LifecycleTransactionGeneration, ControllableComponentId,
                    ControlDomainId, ControlDomainRevision, ProviderInstanceId, ProviderManifestId,
                    ProviderManifestRevision, RuntimeUnitId, RuntimeEpoch, LifecyclePrimitiveId,
                    LifecyclePrimitiveAttemptId, std::uint64_t, std::int64_t,
                    ComponentLifecycleStateDTO>(),
           py::arg("lifecycle_transaction_id"), py::arg("lifecycle_transaction_generation"),
           py::arg("controllable_component_id"), py::arg("control_domain_id"),
           py::arg("control_domain_revision"), py::arg("provider_instance_id"),
           py::arg("provider_manifest_id"), py::arg("provider_manifest_revision"),
           py::arg("provider_runtime_unit_id"), py::arg("provider_runtime_epoch"),
           py::arg("lifecycle_primitive_id"), py::arg("lifecycle_primitive_attempt_id"),
           py::arg("expected_observation_generation"), py::arg("remaining_duration_ns"),
           py::arg("requested_state"))
      .def_readonly("lifecycle_transaction_id",
                    &BrandLifecyclePrimitiveIntentDTO::lifecycle_transaction_id)
      .def_readonly("lifecycle_transaction_generation",
                    &BrandLifecyclePrimitiveIntentDTO::lifecycle_transaction_generation)
      .def_readonly("controllable_component_id",
                    &BrandLifecyclePrimitiveIntentDTO::controllable_component_id)
      .def_readonly("control_domain_id", &BrandLifecyclePrimitiveIntentDTO::control_domain_id)
      .def_readonly("control_domain_revision",
                    &BrandLifecyclePrimitiveIntentDTO::control_domain_revision)
      .def_readonly("provider_instance_id", &BrandLifecyclePrimitiveIntentDTO::provider_instance_id)
      .def_readonly("provider_manifest_id", &BrandLifecyclePrimitiveIntentDTO::provider_manifest_id)
      .def_readonly("provider_manifest_revision",
                    &BrandLifecyclePrimitiveIntentDTO::provider_manifest_revision)
      .def_readonly("provider_runtime_unit_id",
                    &BrandLifecyclePrimitiveIntentDTO::provider_runtime_unit_id)
      .def_readonly("provider_runtime_epoch",
                    &BrandLifecyclePrimitiveIntentDTO::provider_runtime_epoch)
      .def_readonly("lifecycle_primitive_id",
                    &BrandLifecyclePrimitiveIntentDTO::lifecycle_primitive_id)
      .def_readonly("lifecycle_primitive_attempt_id",
                    &BrandLifecyclePrimitiveIntentDTO::lifecycle_primitive_attempt_id)
      .def_readonly("expected_observation_generation",
                    &BrandLifecyclePrimitiveIntentDTO::expected_observation_generation)
      .def_readonly("remaining_duration_ns",
                    &BrandLifecyclePrimitiveIntentDTO::remaining_duration_ns)
      .def_readonly("requested_state", &BrandLifecyclePrimitiveIntentDTO::requested_state)
      .def("locally_valid", &BrandLifecyclePrimitiveIntentDTO::locally_valid)
      .def(py::self == py::self)
      .def(py::self != py::self);

  py::class_<LifecycleSubstepResultDTO>(module, "LifecycleSubstepResultDTO", py::is_final())
      .def(py::init<LifecyclePrimitiveId, std::optional<LifecyclePrimitiveId>,
                    LifecycleTransitionOutcomeDTO, std::int64_t, std::int64_t,
                    std::optional<ErrorDTO>>(),
           py::arg("substep_id"), py::arg("within"), py::arg("outcome"), py::arg("started_at_ns"),
           py::arg("finished_at_ns"), py::arg("error") = std::nullopt)
      .def_readonly("substep_id", &LifecycleSubstepResultDTO::substep_id)
      .def_readonly("within", &LifecycleSubstepResultDTO::within)
      .def_readonly("outcome", &LifecycleSubstepResultDTO::outcome)
      .def_readonly("started_at_ns", &LifecycleSubstepResultDTO::started_at_ns)
      .def_readonly("finished_at_ns", &LifecycleSubstepResultDTO::finished_at_ns)
      .def_readonly("error", &LifecycleSubstepResultDTO::error)
      .def("locally_valid", &LifecycleSubstepResultDTO::locally_valid)
      .def(py::self == py::self)
      .def(py::self != py::self);

  py::class_<LifecycleSubstepEventDTO> substep_event(module, "LifecycleSubstepEventDTO",
                                                     py::is_final());
  py::enum_<LifecycleSubstepEventDTO::Phase>(substep_event, "Phase")
      .value("kStarted", LifecycleSubstepEventDTO::Phase::kStarted)
      .value("kFinished", LifecycleSubstepEventDTO::Phase::kFinished);
  substep_event
      .def(py::init<LifecyclePrimitiveAttemptId, LifecyclePrimitiveId,
                    std::optional<LifecyclePrimitiveId>, LifecycleSubstepEventDTO::Phase,
                    std::int64_t, std::optional<LifecycleTransitionOutcomeDTO>,
                    std::optional<std::string>>(),
           py::arg("attempt_id"), py::arg("substep_id"), py::arg("within"), py::arg("phase"),
           py::arg("at_ns"), py::arg("outcome") = std::nullopt,
           py::arg("error_code") = std::nullopt)
      .def_readonly("attempt_id", &LifecycleSubstepEventDTO::attempt_id)
      .def_readonly("substep_id", &LifecycleSubstepEventDTO::substep_id)
      .def_readonly("within", &LifecycleSubstepEventDTO::within)
      .def_readonly("phase", &LifecycleSubstepEventDTO::phase)
      .def_readonly("at_ns", &LifecycleSubstepEventDTO::at_ns)
      .def_readonly("outcome", &LifecycleSubstepEventDTO::outcome)
      .def_readonly("error_code", &LifecycleSubstepEventDTO::error_code)
      .def("locally_valid", &LifecycleSubstepEventDTO::locally_valid)
      .def(py::self == py::self)
      .def(py::self != py::self);

  py::class_<LifecycleTransitionObservationDTO>(module, "LifecycleTransitionObservationDTO",
                                                py::is_final())
      .def(py::init<ComponentLifecycleStateDTO, std::vector<LifecycleFactDTO>, std::uint64_t,
                    std::uint64_t, std::int64_t>(),
           py::arg("observed_state"), py::arg("facts"), py::arg("observation_generation"),
           py::arg("source_sequence"), py::arg("at_ns"))
      .def_readonly("observed_state", &LifecycleTransitionObservationDTO::observed_state)
      .def_property_readonly("facts",
                             [](const LifecycleTransitionObservationDTO& value) {
                               return immutable_snapshot(value.facts);
                             })
      .def_readonly("observation_generation",
                    &LifecycleTransitionObservationDTO::observation_generation)
      .def_readonly("source_sequence", &LifecycleTransitionObservationDTO::source_sequence)
      .def_readonly("at_ns", &LifecycleTransitionObservationDTO::at_ns)
      .def("locally_valid", &LifecycleTransitionObservationDTO::locally_valid)
      .def(py::self == py::self)
      .def(py::self != py::self);

  py::class_<BrandLifecyclePrimitiveResultDTO>(module, "BrandLifecyclePrimitiveResultDTO",
                                               py::is_final())
      .def(py::init<LifecycleTransactionId, LifecycleTransactionGeneration, ControllableComponentId,
                    ControlDomainId, ControlDomainRevision, ProviderInstanceId, ProviderManifestId,
                    ProviderManifestRevision, RuntimeUnitId, RuntimeEpoch, LifecyclePrimitiveId,
                    LifecyclePrimitiveAttemptId, std::uint64_t, LifecycleTransitionOutcomeDTO,
                    std::optional<ErrorDTO>, std::vector<LifecycleSubstepResultDTO>>(),
           py::arg("lifecycle_transaction_id"), py::arg("lifecycle_transaction_generation"),
           py::arg("controllable_component_id"), py::arg("control_domain_id"),
           py::arg("control_domain_revision"), py::arg("provider_instance_id"),
           py::arg("provider_manifest_id"), py::arg("provider_manifest_revision"),
           py::arg("provider_runtime_unit_id"), py::arg("provider_runtime_epoch"),
           py::arg("lifecycle_primitive_id"), py::arg("lifecycle_primitive_attempt_id"),
           py::arg("expected_observation_generation"), py::arg("outcome"),
           py::arg("error") = std::nullopt,
           py::arg("substeps") = std::vector<LifecycleSubstepResultDTO>{})
      .def_readonly("lifecycle_transaction_id",
                    &BrandLifecyclePrimitiveResultDTO::lifecycle_transaction_id)
      .def_readonly("lifecycle_transaction_generation",
                    &BrandLifecyclePrimitiveResultDTO::lifecycle_transaction_generation)
      .def_readonly("controllable_component_id",
                    &BrandLifecyclePrimitiveResultDTO::controllable_component_id)
      .def_readonly("control_domain_id", &BrandLifecyclePrimitiveResultDTO::control_domain_id)
      .def_readonly("control_domain_revision",
                    &BrandLifecyclePrimitiveResultDTO::control_domain_revision)
      .def_readonly("provider_instance_id", &BrandLifecyclePrimitiveResultDTO::provider_instance_id)
      .def_readonly("provider_manifest_id", &BrandLifecyclePrimitiveResultDTO::provider_manifest_id)
      .def_readonly("provider_manifest_revision",
                    &BrandLifecyclePrimitiveResultDTO::provider_manifest_revision)
      .def_readonly("provider_runtime_unit_id",
                    &BrandLifecyclePrimitiveResultDTO::provider_runtime_unit_id)
      .def_readonly("provider_runtime_epoch",
                    &BrandLifecyclePrimitiveResultDTO::provider_runtime_epoch)
      .def_readonly("lifecycle_primitive_id",
                    &BrandLifecyclePrimitiveResultDTO::lifecycle_primitive_id)
      .def_readonly("lifecycle_primitive_attempt_id",
                    &BrandLifecyclePrimitiveResultDTO::lifecycle_primitive_attempt_id)
      .def_readonly("expected_observation_generation",
                    &BrandLifecyclePrimitiveResultDTO::expected_observation_generation)
      .def_readonly("outcome", &BrandLifecyclePrimitiveResultDTO::outcome)
      .def_readonly("error", &BrandLifecyclePrimitiveResultDTO::error)
      .def_property_readonly("substeps",
                             [](const BrandLifecyclePrimitiveResultDTO& value) {
                               return immutable_snapshot(value.substeps);
                             })
      .def("locally_valid", &BrandLifecyclePrimitiveResultDTO::locally_valid)
      .def("matches", &BrandLifecyclePrimitiveResultDTO::matches)
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

  py::class_<ComponentLifecycleDomainStatusDTO>(module, "ComponentLifecycleDomainStatusDTO",
                                                py::is_final())
      .def(py::init<ControlDomainId, std::optional<ExpectedControlDomainRevisionDTO>,
                    std::optional<ResolvedLifecycleControlDomainScopeDTO>,
                    std::optional<ResolvedLifecycleProviderOccurrenceDTO>,
                    std::optional<ComponentLifecycleStateDTO>,
                    std::optional<ComponentLifecycleStateDTO>, ComponentLifecycleRequestedStateDTO,
                    ComponentLifecycleDomainActionStateDTO,
                    std::vector<BrandLifecyclePrimitiveResultDTO>, std::optional<ErrorDTO>,
                    std::vector<LifecycleSubstepEventDTO>,
                    std::optional<LifecycleTransitionObservationDTO>,
                    std::optional<LifecycleTransitionObservationDTO>>(),
           py::arg("control_domain_id"), py::arg("expected_domain"), py::arg("current_scope"),
           py::arg("resolved_provider_occurrence"), py::arg("previous_state"),
           py::arg("current_state"), py::arg("requested_state"), py::arg("action_state"),
           py::arg("primitive_attempts"), py::arg("error"),
           py::arg("substep_events") = std::vector<LifecycleSubstepEventDTO>{},
           py::arg("observed_before") = std::nullopt, py::arg("observed_after") = std::nullopt)
      .def_readonly("control_domain_id", &ComponentLifecycleDomainStatusDTO::control_domain_id)
      .def_readonly("expected_domain", &ComponentLifecycleDomainStatusDTO::expected_domain)
      .def_readonly("current_scope", &ComponentLifecycleDomainStatusDTO::current_scope)
      .def_readonly("resolved_provider_occurrence",
                    &ComponentLifecycleDomainStatusDTO::resolved_provider_occurrence)
      .def_readonly("previous_state", &ComponentLifecycleDomainStatusDTO::previous_state)
      .def_readonly("current_state", &ComponentLifecycleDomainStatusDTO::current_state)
      .def_readonly("requested_state", &ComponentLifecycleDomainStatusDTO::requested_state)
      .def_readonly("action_state", &ComponentLifecycleDomainStatusDTO::action_state)
      .def_property_readonly("primitive_attempts",
                             [](const ComponentLifecycleDomainStatusDTO& value) {
                               return immutable_snapshot(value.primitive_attempts);
                             })
      .def_readonly("error", &ComponentLifecycleDomainStatusDTO::error)
      .def_property_readonly("substep_events",
                             [](const ComponentLifecycleDomainStatusDTO& value) {
                               return immutable_snapshot(value.substep_events);
                             })
      .def_readonly("observed_before", &ComponentLifecycleDomainStatusDTO::observed_before)
      .def_readonly("observed_after", &ComponentLifecycleDomainStatusDTO::observed_after)
      .def("locally_valid", &ComponentLifecycleDomainStatusDTO::locally_valid)
      .def(py::self == py::self)
      .def(py::self != py::self);

  py::class_<ComponentLifecycleStatusDTO>(module, "ComponentLifecycleStatusDTO", py::is_final())
      .def(py::init<ComponentLifecycleAcceptedDTO, ComponentLifecycleIntentDTO, std::uint64_t,
                    RequestOwnerEpoch, std::optional<ComponentLifecycleStateDTO>,
                    std::optional<ComponentLifecycleStateDTO>, ComponentLifecycleRequestedStateDTO,
                    ComponentLifecycleTransactionPhaseDTO,
                    std::vector<ComponentLifecycleDomainStatusDTO>, bool, std::optional<ComponentLifecycleTerminalResultDTO>,
                    std::optional<ErrorDTO>, std::int64_t, std::int64_t, std::int64_t>(),
           py::arg("acceptance"), py::arg("accepted_intent"), py::arg("status_sequence"),
           py::arg("status_owner_epoch"), py::arg("previous_state"), py::arg("current_state"),
           py::arg("requested_state"), py::arg("phase"), py::arg("domains"),
           py::arg("reconciliation_required"),
           py::arg("terminal_result"), py::arg("error"), py::arg("accepted_at_ns"),
           py::arg("latest_updated_at_ns"), py::arg("remaining_duration_ns"))
      .def_readonly("acceptance", &ComponentLifecycleStatusDTO::acceptance)
      .def_readonly("accepted_intent", &ComponentLifecycleStatusDTO::accepted_intent)
      .def_readonly("status_sequence", &ComponentLifecycleStatusDTO::status_sequence)
      .def_readonly("status_owner_epoch", &ComponentLifecycleStatusDTO::status_owner_epoch)
      .def_readonly("previous_state", &ComponentLifecycleStatusDTO::previous_state)
      .def_readonly("current_state", &ComponentLifecycleStatusDTO::current_state)
      .def_readonly("requested_state", &ComponentLifecycleStatusDTO::requested_state)
      .def_readonly("phase", &ComponentLifecycleStatusDTO::phase)
      .def_property_readonly("domains",
                             [](const ComponentLifecycleStatusDTO& value) {
                               return immutable_snapshot(value.domains);
                             })
      .def_readonly("reconciliation_required",
                    &ComponentLifecycleStatusDTO::reconciliation_required)
      .def_readonly("terminal_result", &ComponentLifecycleStatusDTO::terminal_result)
      .def_readonly("error", &ComponentLifecycleStatusDTO::error)
      .def_readonly("accepted_at_ns", &ComponentLifecycleStatusDTO::accepted_at_ns)
      .def_readonly("latest_updated_at_ns", &ComponentLifecycleStatusDTO::latest_updated_at_ns)
      .def_readonly("remaining_duration_ns", &ComponentLifecycleStatusDTO::remaining_duration_ns)
      .def("locally_valid", &ComponentLifecycleStatusDTO::locally_valid)
      .def(py::self == py::self)
      .def(py::self != py::self);

  py::class_<CCLCLifecycleRequestResultDTO>(module, "CCLCLifecycleRequestResultDTO", py::is_final())
      .def(py::init<RequestReplayStatusDTO, std::optional<ComponentLifecycleAcceptedDTO>,
                    std::optional<ComponentLifecycleStatusDTO>, std::optional<ErrorDTO>>(),
           py::arg("replay_status"), py::arg("acceptance") = std::nullopt,
           py::arg("status") = std::nullopt, py::arg("rejection") = std::nullopt)
      .def_readonly("replay_status", &CCLCLifecycleRequestResultDTO::replay_status)
      .def_readonly("acceptance", &CCLCLifecycleRequestResultDTO::acceptance)
      .def_readonly("status", &CCLCLifecycleRequestResultDTO::status)
      .def_readonly("rejection", &CCLCLifecycleRequestResultDTO::rejection)
      .def("locally_valid", &CCLCLifecycleRequestResultDTO::locally_valid);

  py::class_<RouteSelectionComponentExpectationDTO>(module, "RouteSelectionComponentExpectationDTO",
                                                    py::is_final())
      .def(py::init<ControllableComponentId, ComponentLifecycleGeneration,
                    ComponentLifecycleStateDTO>(),
           py::arg("controllable_component_id"), py::arg("expected_component_lifecycle_generation"),
           py::arg("expected_component_lifecycle_state"))
      .def_readonly("controllable_component_id",
                    &RouteSelectionComponentExpectationDTO::controllable_component_id)
      .def_readonly("expected_component_lifecycle_generation",
                    &RouteSelectionComponentExpectationDTO::expected_component_lifecycle_generation)
      .def_readonly("expected_component_lifecycle_state",
                    &RouteSelectionComponentExpectationDTO::expected_component_lifecycle_state)
      .def("locally_valid", &RouteSelectionComponentExpectationDTO::locally_valid)
      .def(py::self == py::self);

  py::class_<RouteSelectionIntentDTO>(module, "RouteSelectionIntentDTO", py::is_final())
      .def(py::init<PublicRequestContextDTO, RetainedBundleId, GraphRevision,
                    DesiredControlConfigurationDTO, ExpectedActiveControlConfigurationDTO,
                    std::vector<RouteSelectionComponentExpectationDTO>, RequestAttributionDTO,
                    std::optional<AdmissionGeneration>,
                    std::optional<ProgramNodeAdmissionEvidenceDTO>>(),
           py::arg("request_context"), py::arg("expected_retained_bundle_id"),
           py::arg("expected_graph_revision"), py::arg("desired_configuration"),
           py::arg("expected_active_control_configuration"), py::arg("component_expectations"),
           py::arg("attribution"), py::arg("expected_admission_generation") = std::nullopt,
           py::arg("program_node_evidence") = std::nullopt)
      .def_readonly("request_context", &RouteSelectionIntentDTO::request_context)
      .def_readonly("expected_retained_bundle_id",
                    &RouteSelectionIntentDTO::expected_retained_bundle_id)
      .def_readonly("expected_graph_revision", &RouteSelectionIntentDTO::expected_graph_revision)
      .def_readonly("desired_configuration", &RouteSelectionIntentDTO::desired_configuration)
      .def_readonly("expected_active_control_configuration",
                    &RouteSelectionIntentDTO::expected_active_control_configuration)
      .def_property_readonly("component_expectations",
                             [](const RouteSelectionIntentDTO& value) {
                               return immutable_snapshot(value.component_expectations);
                             })
      .def_readonly("attribution", &RouteSelectionIntentDTO::attribution)
      .def_readonly("expected_admission_generation",
                    &RouteSelectionIntentDTO::expected_admission_generation)
      .def_readonly("program_node_evidence", &RouteSelectionIntentDTO::program_node_evidence)
      .def("locally_valid", &RouteSelectionIntentDTO::locally_valid)
      .def("replay_equal", &RouteSelectionIntentDTO::replay_equal)
      .def(py::self == py::self);
}

}  // namespace manyforge::core::python
