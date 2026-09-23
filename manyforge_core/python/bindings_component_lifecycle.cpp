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

  py::enum_<LifecyclePrimitiveDispatchStateDTO>(module, "LifecyclePrimitiveDispatchStateDTO")
      .value("kNotDispatched", LifecyclePrimitiveDispatchStateDTO::kNotDispatched)
      .value("kDispatchedKnown", LifecyclePrimitiveDispatchStateDTO::kDispatchedKnown)
      .value("kDispatchedUncertain", LifecyclePrimitiveDispatchStateDTO::kDispatchedUncertain);

  py::enum_<LifecyclePrimitiveKnownResultDTO>(module, "LifecyclePrimitiveKnownResultDTO")
      .value("kSucceeded", LifecyclePrimitiveKnownResultDTO::kSucceeded)
      .value("kFailed", LifecyclePrimitiveKnownResultDTO::kFailed);

  py::enum_<LifecyclePrimitiveEffectDTO>(module, "LifecyclePrimitiveEffectDTO")
      .value("kNone", LifecyclePrimitiveEffectDTO::kNone)
      .value("kPossible", LifecyclePrimitiveEffectDTO::kPossible)
      .value("kObserved", LifecyclePrimitiveEffectDTO::kObserved);

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
      .value("kReconciling", ComponentLifecycleDomainActionStateDTO::kReconciling)
      .value("kSucceeded", ComponentLifecycleDomainActionStateDTO::kSucceeded)
      .value("kFailed", ComponentLifecycleDomainActionStateDTO::kFailed)
      .value("kOutcomeUnknown", ComponentLifecycleDomainActionStateDTO::kOutcomeUnknown);

  py::enum_<ComponentLifecycleDomainEffectDTO>(module, "ComponentLifecycleDomainEffectDTO")
      .value("kNone", ComponentLifecycleDomainEffectDTO::kNone)
      .value("kKnownNoEffect", ComponentLifecycleDomainEffectDTO::kKnownNoEffect)
      .value("kEffectObserved", ComponentLifecycleDomainEffectDTO::kEffectObserved)
      .value("kPartialEffectObserved", ComponentLifecycleDomainEffectDTO::kPartialEffectObserved)
      .value("kEffectUncertain", ComponentLifecycleDomainEffectDTO::kEffectUncertain);

  py::enum_<ComponentLifecycleTransactionPhaseDTO>(module, "ComponentLifecycleTransactionPhaseDTO")
      .value("kAccepted", ComponentLifecycleTransactionPhaseDTO::kAccepted)
      .value("kAcquiringDomains", ComponentLifecycleTransactionPhaseDTO::kAcquiringDomains)
      .value("kContaining", ComponentLifecycleTransactionPhaseDTO::kContaining)
      .value("kExecuting", ComponentLifecycleTransactionPhaseDTO::kExecuting)
      .value("kVerifying", ComponentLifecycleTransactionPhaseDTO::kVerifying)
      .value("kOutcomeUnknown", ComponentLifecycleTransactionPhaseDTO::kOutcomeUnknown)
      .value("kReconciling", ComponentLifecycleTransactionPhaseDTO::kReconciling)
      .value("kSucceeded", ComponentLifecycleTransactionPhaseDTO::kSucceeded)
      .value("kFailed", ComponentLifecycleTransactionPhaseDTO::kFailed);

  py::enum_<ComponentLifecycleTerminalResultDTO>(module, "ComponentLifecycleTerminalResultDTO")
      .value("kSucceeded", ComponentLifecycleTerminalResultDTO::kSucceeded)
      .value("kFailedKnownNoEffect", ComponentLifecycleTerminalResultDTO::kFailedKnownNoEffect)
      .value("kFailedPartialEffect", ComponentLifecycleTerminalResultDTO::kFailedPartialEffect);

  py::enum_<ConfigurationFenceState>(module, "ConfigurationFenceState")
      .value("kHeld", ConfigurationFenceState::kHeld)
      .value("kReleasePending", ConfigurationFenceState::kReleasePending)
      .value("kReleased", ConfigurationFenceState::kReleased);

  py::enum_<DisableHandoffStateDTO>(module, "DisableHandoffStateDTO")
      .value("kStopping", DisableHandoffStateDTO::kStopping)
      .value("kReady", DisableHandoffStateDTO::kReady)
      .value("kFailed", DisableHandoffStateDTO::kFailed);

  py::enum_<DisableHandoffAttemptDispositionDTO>(module, "DisableHandoffAttemptDispositionDTO")
      .value("kAcceptedExact", DisableHandoffAttemptDispositionDTO::kAcceptedExact)
      .value("kNotSeen", DisableHandoffAttemptDispositionDTO::kNotSeen)
      .value("kConflicting", DisableHandoffAttemptDispositionDTO::kConflicting)
      .value("kUncertain", DisableHandoffAttemptDispositionDTO::kUncertain);

  py::enum_<DisableHandoffScopeRequestKindDTO>(module, "DisableHandoffScopeRequestKindDTO")
      .value("kResolvedComponentScope", DisableHandoffScopeRequestKindDTO::kResolvedComponentScope)
      .value("kFullStopRequired", DisableHandoffScopeRequestKindDTO::kFullStopRequired);

  py::enum_<DisableHandoffScopeResolutionDTO>(module, "DisableHandoffScopeResolutionDTO")
      .value("kExact", DisableHandoffScopeResolutionDTO::kExact)
      .value("kStaleButUnambiguouslyResolved",
             DisableHandoffScopeResolutionDTO::kStaleButUnambiguouslyResolved)
      .value("kPromotedToFull", DisableHandoffScopeResolutionDTO::kPromotedToFull);

  py::enum_<DisableFenceReleaseAttemptDispositionDTO>(module,
                                                      "DisableFenceReleaseAttemptDispositionDTO")
      .value("kReleasedExact", DisableFenceReleaseAttemptDispositionDTO::kReleasedExact)
      .value("kNotReleasedHeld", DisableFenceReleaseAttemptDispositionDTO::kNotReleasedHeld)
      .value("kConflicting", DisableFenceReleaseAttemptDispositionDTO::kConflicting)
      .value("kUncertain", DisableFenceReleaseAttemptDispositionDTO::kUncertain);

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

  py::class_<ExpectedControlDomainRevisionDTO>(module, "ExpectedControlDomainRevisionDTO",
                                               py::is_final())
      .def(py::init<ControlDomainId, ControlDomainRevision, ControllerTopologyId, TopologyRevision,
                    ExpectedActiveControlConfigurationDTO,
                    std::vector<ExpectedControlBindingRevisionDTO>>(),
           py::arg("control_domain_id"), py::arg("control_domain_revision"),
           py::arg("controller_topology_id"), py::arg("topology_revision"),
           py::arg("expected_active_control_configuration"), py::arg("expected_control_bindings"))
      .def_readonly("control_domain_id", &ExpectedControlDomainRevisionDTO::control_domain_id)
      .def_readonly("control_domain_revision",
                    &ExpectedControlDomainRevisionDTO::control_domain_revision)
      .def_readonly("controller_topology_id",
                    &ExpectedControlDomainRevisionDTO::controller_topology_id)
      .def_readonly("topology_revision", &ExpectedControlDomainRevisionDTO::topology_revision)
      .def_readonly("expected_active_control_configuration",
                    &ExpectedControlDomainRevisionDTO::expected_active_control_configuration)
      .def_property_readonly("expected_control_bindings",
                             [](const ExpectedControlDomainRevisionDTO& value) {
                               return immutable_snapshot(value.expected_control_bindings);
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

  py::class_<LifecycleIntentPlanDTO>(module, "LifecycleIntentPlanDTO", py::is_final())
      .def(py::init<ComponentLifecycleIntentKindDTO, ComponentLifecycleStateDTO,
                    std::optional<LifecyclePredicateDTO>, LifecyclePredicateDTO, std::int64_t,
                    std::vector<LifecyclePrimitiveTransitionDTO>>(),
           py::arg("intent"), py::arg("requested_state"), py::arg("recover_target_eligibility"),
           py::arg("requested_postcondition"), py::arg("timeout_duration_ns"),
           py::arg("transitions"))
      .def_readonly("intent", &LifecycleIntentPlanDTO::intent)
      .def_readonly("requested_state", &LifecycleIntentPlanDTO::requested_state)
      .def_readonly("recover_target_eligibility",
                    &LifecycleIntentPlanDTO::recover_target_eligibility)
      .def_readonly("requested_postcondition", &LifecycleIntentPlanDTO::requested_postcondition)
      .def_readonly("timeout_duration_ns", &LifecycleIntentPlanDTO::timeout_duration_ns)
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
                    LifecyclePrimitiveAttemptId, std::uint64_t, std::int64_t>(),
           py::arg("lifecycle_transaction_id"), py::arg("lifecycle_transaction_generation"),
           py::arg("controllable_component_id"), py::arg("control_domain_id"),
           py::arg("control_domain_revision"), py::arg("provider_instance_id"),
           py::arg("provider_manifest_id"), py::arg("provider_manifest_revision"),
           py::arg("provider_runtime_unit_id"), py::arg("provider_runtime_epoch"),
           py::arg("lifecycle_primitive_id"), py::arg("lifecycle_primitive_attempt_id"),
           py::arg("expected_observation_generation"), py::arg("remaining_duration_ns"))
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
      .def("locally_valid", &BrandLifecyclePrimitiveIntentDTO::locally_valid)
      .def(py::self == py::self)
      .def(py::self != py::self);

  py::class_<BrandLifecyclePrimitiveResultDTO>(module, "BrandLifecyclePrimitiveResultDTO",
                                               py::is_final())
      .def(py::init<LifecycleTransactionId, LifecycleTransactionGeneration, ControllableComponentId,
                    ControlDomainId, ControlDomainRevision, ProviderInstanceId, ProviderManifestId,
                    ProviderManifestRevision, RuntimeUnitId, RuntimeEpoch, LifecyclePrimitiveId,
                    LifecyclePrimitiveAttemptId, std::uint64_t, LifecyclePrimitiveDispatchStateDTO,
                    LifecyclePrimitiveEffectDTO, std::optional<LifecyclePrimitiveKnownResultDTO>,
                    std::optional<ErrorDTO>>(),
           py::arg("lifecycle_transaction_id"), py::arg("lifecycle_transaction_generation"),
           py::arg("controllable_component_id"), py::arg("control_domain_id"),
           py::arg("control_domain_revision"), py::arg("provider_instance_id"),
           py::arg("provider_manifest_id"), py::arg("provider_manifest_revision"),
           py::arg("provider_runtime_unit_id"), py::arg("provider_runtime_epoch"),
           py::arg("lifecycle_primitive_id"), py::arg("lifecycle_primitive_attempt_id"),
           py::arg("expected_observation_generation"), py::arg("dispatch_state"), py::arg("effect"),
           py::arg("known_result") = std::nullopt, py::arg("error") = std::nullopt)
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
      .def_readonly("dispatch_state", &BrandLifecyclePrimitiveResultDTO::dispatch_state)
      .def_readonly("effect", &BrandLifecyclePrimitiveResultDTO::effect)
      .def_readonly("known_result", &BrandLifecyclePrimitiveResultDTO::known_result)
      .def_readonly("error", &BrandLifecyclePrimitiveResultDTO::error)
      .def("locally_valid", &BrandLifecyclePrimitiveResultDTO::locally_valid)
      .def("matches", &BrandLifecyclePrimitiveResultDTO::matches)
      .def(py::self == py::self)
      .def(py::self != py::self);

  py::class_<LifecycleReconciliationEvidenceDTO>(module, "LifecycleReconciliationEvidenceDTO",
                                                 py::is_final())
      .def(py::init<ProviderInstanceId, ProviderManifestId, ProviderManifestRevision, RuntimeUnitId,
                    RuntimeEpoch, std::uint64_t, std::uint64_t, ComponentLifecycleStateDTO,
                    LifecyclePostconditionResultDTO, std::int64_t>(),
           py::arg("provider_instance_id"), py::arg("provider_manifest_id"),
           py::arg("provider_manifest_revision"), py::arg("provider_runtime_unit_id"),
           py::arg("provider_runtime_epoch"), py::arg("source_sequence"),
           py::arg("observation_generation"), py::arg("observed_state"),
           py::arg("postcondition_result"), py::arg("observed_at_ns"))
      .def_readonly("provider_instance_id",
                    &LifecycleReconciliationEvidenceDTO::provider_instance_id)
      .def_readonly("provider_manifest_id",
                    &LifecycleReconciliationEvidenceDTO::provider_manifest_id)
      .def_readonly("provider_manifest_revision",
                    &LifecycleReconciliationEvidenceDTO::provider_manifest_revision)
      .def_readonly("provider_runtime_unit_id",
                    &LifecycleReconciliationEvidenceDTO::provider_runtime_unit_id)
      .def_readonly("provider_runtime_epoch",
                    &LifecycleReconciliationEvidenceDTO::provider_runtime_epoch)
      .def_readonly("source_sequence", &LifecycleReconciliationEvidenceDTO::source_sequence)
      .def_readonly("observation_generation",
                    &LifecycleReconciliationEvidenceDTO::observation_generation)
      .def_readonly("observed_state", &LifecycleReconciliationEvidenceDTO::observed_state)
      .def_readonly("postcondition_result",
                    &LifecycleReconciliationEvidenceDTO::postcondition_result)
      .def_readonly("observed_at_ns", &LifecycleReconciliationEvidenceDTO::observed_at_ns)
      .def("locally_valid", &LifecycleReconciliationEvidenceDTO::locally_valid)
      .def(py::self == py::self)
      .def(py::self != py::self);

  py::class_<ResolvedLifecycleControlDomainScopeDTO>(
      module, "ResolvedLifecycleControlDomainScopeDTO", py::is_final())
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
      .def_readonly("controller_topology_id",
                    &ResolvedLifecycleControlDomainScopeDTO::controller_topology_id)
      .def_readonly("topology_revision", &ResolvedLifecycleControlDomainScopeDTO::topology_revision)
      .def_readonly("active_control_configuration",
                    &ResolvedLifecycleControlDomainScopeDTO::active_control_configuration)
      .def_property_readonly("current_control_bindings",
                             [](const ResolvedLifecycleControlDomainScopeDTO& value) {
                               return immutable_snapshot(value.current_control_bindings);
                             })
      .def_property_readonly("current_resources",
                             [](const ResolvedLifecycleControlDomainScopeDTO& value) {
                               return immutable_snapshot(value.current_resources);
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

  py::class_<ConfigurationFenceDomainScopeDTO>(module, "ConfigurationFenceDomainScopeDTO",
                                               py::is_final())
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

  py::class_<ConfigurationFenceHandleDTO>(module, "ConfigurationFenceHandleDTO", py::is_final())
      .def(py::init<ConfigurationFenceId, ConfigurationFenceGeneration, LifecycleTransactionId,
                    LifecycleTransactionGeneration, ControllableComponentId,
                    ConfigurationFenceScopeDTO, std::string, ConfigurationFenceState>(),
           py::arg("configuration_fence_id"), py::arg("configuration_fence_generation"),
           py::arg("lifecycle_transaction_id"), py::arg("lifecycle_transaction_generation"),
           py::arg("controllable_component_id"), py::arg("scope"), py::arg("reason"),
           py::arg("state"))
      .def_readonly("configuration_fence_id", &ConfigurationFenceHandleDTO::configuration_fence_id)
      .def_readonly("configuration_fence_generation",
                    &ConfigurationFenceHandleDTO::configuration_fence_generation)
      .def_readonly("lifecycle_transaction_id",
                    &ConfigurationFenceHandleDTO::lifecycle_transaction_id)
      .def_readonly("lifecycle_transaction_generation",
                    &ConfigurationFenceHandleDTO::lifecycle_transaction_generation)
      .def_readonly("controllable_component_id",
                    &ConfigurationFenceHandleDTO::controllable_component_id)
      .def_readonly("scope", &ConfigurationFenceHandleDTO::scope)
      .def_readonly("reason", &ConfigurationFenceHandleDTO::reason)
      .def_readonly("state", &ConfigurationFenceHandleDTO::state)
      .def("locally_valid", &ConfigurationFenceHandleDTO::locally_valid)
      .def(py::self == py::self)
      .def(py::self != py::self);

  py::class_<CurrentDomainActiveControlConfigurationDTO>(
      module, "CurrentDomainActiveControlConfigurationDTO", py::is_final())
      .def(py::init<ControlDomainId, ExpectedActiveControlConfigurationDTO>(),
           py::arg("control_domain_id"), py::arg("active_control_configuration"))
      .def_readonly("control_domain_id",
                    &CurrentDomainActiveControlConfigurationDTO::control_domain_id)
      .def_readonly("active_control_configuration",
                    &CurrentDomainActiveControlConfigurationDTO::active_control_configuration)
      .def("locally_valid", &CurrentDomainActiveControlConfigurationDTO::locally_valid)
      .def(py::self == py::self)
      .def(py::self != py::self);

  py::class_<DisableHandoffComponentScopeExpectationDTO>(
      module, "DisableHandoffComponentScopeExpectationDTO", py::is_final())
      .def(py::init<ContentDigest, ActiveBundlePublicationGeneration, ContentDigest,
                    RetainedBundleId, GraphRevision, ComponentLifecycleGeneration,
                    ConfigurationFenceScopeDTO,
                    std::vector<CurrentDomainActiveControlConfigurationDTO>>(),
           py::arg("component_snapshot_digest"), py::arg("active_publication_generation"),
           py::arg("active_public_pointer_digest"), py::arg("retained_bundle_id"),
           py::arg("graph_revision"), py::arg("component_lifecycle_generation"),
           py::arg("component_scope"), py::arg("current_active_control_configurations"))
      .def_readonly("component_snapshot_digest",
                    &DisableHandoffComponentScopeExpectationDTO::component_snapshot_digest)
      .def_readonly("active_publication_generation",
                    &DisableHandoffComponentScopeExpectationDTO::active_publication_generation)
      .def_readonly("active_public_pointer_digest",
                    &DisableHandoffComponentScopeExpectationDTO::active_public_pointer_digest)
      .def_readonly("retained_bundle_id",
                    &DisableHandoffComponentScopeExpectationDTO::retained_bundle_id)
      .def_readonly("graph_revision", &DisableHandoffComponentScopeExpectationDTO::graph_revision)
      .def_readonly("component_lifecycle_generation",
                    &DisableHandoffComponentScopeExpectationDTO::component_lifecycle_generation)
      .def_readonly("component_scope", &DisableHandoffComponentScopeExpectationDTO::component_scope)
      .def_property_readonly(
          "current_active_control_configurations",
          [](const DisableHandoffComponentScopeExpectationDTO& value) {
            return immutable_snapshot(value.current_active_control_configurations);
          })
      .def("locally_valid", &DisableHandoffComponentScopeExpectationDTO::locally_valid)
      .def(py::self == py::self)
      .def(py::self != py::self);

  py::class_<DisableHandoffAdmissionQueryDTO>(module, "DisableHandoffAdmissionQueryDTO",
                                              py::is_final())
      .def(py::init<ControllableComponentId, DisableHandoffScopeRequestKindDTO,
                    std::optional<DisableHandoffComponentScopeExpectationDTO>>(),
           py::arg("controllable_component_id"), py::arg("request_kind"),
           py::arg("component_expectation") = std::nullopt)
      .def_readonly("controllable_component_id",
                    &DisableHandoffAdmissionQueryDTO::controllable_component_id)
      .def_readonly("request_kind", &DisableHandoffAdmissionQueryDTO::request_kind)
      .def_readonly("component_expectation",
                    &DisableHandoffAdmissionQueryDTO::component_expectation)
      .def("locally_valid", &DisableHandoffAdmissionQueryDTO::locally_valid)
      .def(py::self == py::self)
      .def(py::self != py::self);

  py::class_<DisableHandoffAdmissionSnapshotDTO>(module, "DisableHandoffAdmissionSnapshotDTO",
                                                 py::is_final())
      .def(py::init<DisableHandoffAdmissionQueryDTO, RequestOwnerEpoch,
                    DisableHandoffScopeResolutionDTO, ConfigurationFenceScopeDTO,
                    ActiveBundlePublicationGeneration, ContentDigest, RetainedBundleId,
                    GraphRevision, std::uint64_t, ConfigurationFenceGeneration,
                    std::vector<CurrentDomainActiveControlConfigurationDTO>, ContentDigest>(),
           py::arg("query"), py::arg("request_owner_epoch"), py::arg("scope_resolution"),
           py::arg("current_scope"), py::arg("active_publication_generation"),
           py::arg("active_public_pointer_digest"), py::arg("retained_bundle_id"),
           py::arg("graph_revision"), py::arg("operation_resource_generation"),
           py::arg("configuration_fence_generation"),
           py::arg("current_active_control_configurations"), py::arg("snapshot_digest"))
      .def_readonly("query", &DisableHandoffAdmissionSnapshotDTO::query)
      .def_readonly("request_owner_epoch", &DisableHandoffAdmissionSnapshotDTO::request_owner_epoch)
      .def_readonly("scope_resolution", &DisableHandoffAdmissionSnapshotDTO::scope_resolution)
      .def_readonly("current_scope", &DisableHandoffAdmissionSnapshotDTO::current_scope)
      .def_readonly("active_publication_generation",
                    &DisableHandoffAdmissionSnapshotDTO::active_publication_generation)
      .def_readonly("active_public_pointer_digest",
                    &DisableHandoffAdmissionSnapshotDTO::active_public_pointer_digest)
      .def_readonly("retained_bundle_id", &DisableHandoffAdmissionSnapshotDTO::retained_bundle_id)
      .def_readonly("graph_revision", &DisableHandoffAdmissionSnapshotDTO::graph_revision)
      .def_readonly("operation_resource_generation",
                    &DisableHandoffAdmissionSnapshotDTO::operation_resource_generation)
      .def_readonly("configuration_fence_generation",
                    &DisableHandoffAdmissionSnapshotDTO::configuration_fence_generation)
      .def_property_readonly(
          "current_active_control_configurations",
          [](const DisableHandoffAdmissionSnapshotDTO& value) {
            return immutable_snapshot(value.current_active_control_configurations);
          })
      .def_readonly("snapshot_digest", &DisableHandoffAdmissionSnapshotDTO::snapshot_digest)
      .def("locally_valid", &DisableHandoffAdmissionSnapshotDTO::locally_valid)
      .def(py::self == py::self)
      .def(py::self != py::self);

  py::class_<DisableHandoffRequestDTO>(module, "DisableHandoffRequestDTO", py::is_final())
      .def(py::init<PublicRequestContextDTO, RequestId, LifecycleTransactionId,
                    LifecycleTransactionGeneration, ControllableComponentId,
                    DisableHandoffScopeResolutionDTO, ConfigurationFenceScopeDTO, ContentDigest,
                    ActiveBundlePublicationGeneration, ContentDigest, GraphRevision,
                    RetainedBundleId, ComponentLifecycleGeneration, std::uint64_t,
                    ConfigurationFenceGeneration,
                    std::vector<CurrentDomainActiveControlConfigurationDTO>>(),
           py::arg("owner_request_context"), py::arg("initiating_public_request_id"),
           py::arg("lifecycle_transaction_id"), py::arg("lifecycle_transaction_generation"),
           py::arg("controllable_component_id"), py::arg("scope_resolution"),
           py::arg("affected_scope"), py::arg("disable_admission_snapshot_digest"),
           py::arg("expected_active_publication_generation"),
           py::arg("expected_active_public_pointer_digest"), py::arg("expected_graph_revision"),
           py::arg("expected_retained_bundle_id"),
           py::arg("expected_component_lifecycle_generation"),
           py::arg("expected_operation_resource_generation"), py::arg("expected_fence_generation"),
           py::arg("expected_active_control_configurations"))
      .def_readonly("owner_request_context", &DisableHandoffRequestDTO::owner_request_context)
      .def_readonly("initiating_public_request_id",
                    &DisableHandoffRequestDTO::initiating_public_request_id)
      .def_readonly("lifecycle_transaction_id", &DisableHandoffRequestDTO::lifecycle_transaction_id)
      .def_readonly("lifecycle_transaction_generation",
                    &DisableHandoffRequestDTO::lifecycle_transaction_generation)
      .def_readonly("controllable_component_id",
                    &DisableHandoffRequestDTO::controllable_component_id)
      .def_readonly("scope_resolution", &DisableHandoffRequestDTO::scope_resolution)
      .def_readonly("affected_scope", &DisableHandoffRequestDTO::affected_scope)
      .def_readonly("disable_admission_snapshot_digest",
                    &DisableHandoffRequestDTO::disable_admission_snapshot_digest)
      .def_readonly("expected_active_publication_generation",
                    &DisableHandoffRequestDTO::expected_active_publication_generation)
      .def_readonly("expected_active_public_pointer_digest",
                    &DisableHandoffRequestDTO::expected_active_public_pointer_digest)
      .def_readonly("expected_graph_revision", &DisableHandoffRequestDTO::expected_graph_revision)
      .def_readonly("expected_retained_bundle_id",
                    &DisableHandoffRequestDTO::expected_retained_bundle_id)
      .def_readonly("expected_component_lifecycle_generation",
                    &DisableHandoffRequestDTO::expected_component_lifecycle_generation)
      .def_readonly("expected_operation_resource_generation",
                    &DisableHandoffRequestDTO::expected_operation_resource_generation)
      .def_readonly("expected_fence_generation",
                    &DisableHandoffRequestDTO::expected_fence_generation)
      .def_property_readonly(
          "expected_active_control_configurations",
          [](const DisableHandoffRequestDTO& value) {
            return immutable_snapshot(value.expected_active_control_configurations);
          })
      .def("locally_valid", &DisableHandoffRequestDTO::locally_valid)
      .def(py::self == py::self)
      .def(py::self != py::self);

  py::class_<DisableHandoffAcceptedDTO>(module, "DisableHandoffAcceptedDTO", py::is_final())
      .def(py::init<RequestId, RequestReplayStatusDTO, LifecycleTransactionId,
                    LifecycleTransactionGeneration, ControllableComponentId,
                    DisableHandoffScopeResolutionDTO, ContentDigest, ConfigurationFenceId,
                    ConfigurationFenceGeneration, std::optional<StopId>,
                    std::optional<StopGeneration>>(),
           py::arg("request_id"), py::arg("replay_status"), py::arg("lifecycle_transaction_id"),
           py::arg("lifecycle_transaction_generation"), py::arg("controllable_component_id"),
           py::arg("scope_resolution"), py::arg("component_scope_digest"),
           py::arg("configuration_fence_id"), py::arg("configuration_fence_generation"),
           py::arg("stop_id") = std::nullopt, py::arg("stop_generation") = std::nullopt)
      .def_readonly("request_id", &DisableHandoffAcceptedDTO::request_id)
      .def_readonly("replay_status", &DisableHandoffAcceptedDTO::replay_status)
      .def_readonly("lifecycle_transaction_id",
                    &DisableHandoffAcceptedDTO::lifecycle_transaction_id)
      .def_readonly("lifecycle_transaction_generation",
                    &DisableHandoffAcceptedDTO::lifecycle_transaction_generation)
      .def_readonly("controllable_component_id",
                    &DisableHandoffAcceptedDTO::controllable_component_id)
      .def_readonly("scope_resolution", &DisableHandoffAcceptedDTO::scope_resolution)
      .def_readonly("component_scope_digest", &DisableHandoffAcceptedDTO::component_scope_digest)
      .def_readonly("configuration_fence_id", &DisableHandoffAcceptedDTO::configuration_fence_id)
      .def_readonly("configuration_fence_generation",
                    &DisableHandoffAcceptedDTO::configuration_fence_generation)
      .def_readonly("stop_id", &DisableHandoffAcceptedDTO::stop_id)
      .def_readonly("stop_generation", &DisableHandoffAcceptedDTO::stop_generation)
      .def("locally_valid", &DisableHandoffAcceptedDTO::locally_valid)
      .def("matches", &DisableHandoffAcceptedDTO::matches)
      .def(py::self == py::self)
      .def(py::self != py::self);

  py::class_<DisableHandoffStatusDTO>(module, "DisableHandoffStatusDTO", py::is_final())
      .def(
          py::init<LifecycleTransactionId, LifecycleTransactionGeneration, ControllableComponentId,
                   DisableHandoffScopeResolutionDTO, ContentDigest, ConfigurationFenceId,
                   ConfigurationFenceGeneration, DisableHandoffStateDTO, ConfigurationFenceScopeDTO,
                   ConfigurationFenceHandleDTO, std::optional<StopCertificateRefDTO>,
                   std::vector<OperationId>, std::optional<ErrorDTO>>(),
          py::arg("lifecycle_transaction_id"), py::arg("lifecycle_transaction_generation"),
          py::arg("controllable_component_id"), py::arg("scope_resolution"),
          py::arg("component_scope_digest"), py::arg("configuration_fence_id"),
          py::arg("configuration_fence_generation"), py::arg("state"), py::arg("scope"),
          py::arg("configuration_fence_handle"), py::arg("stop_certificate") = std::nullopt,
          py::arg("cancelled_queued_operations") = std::vector<OperationId>{},
          py::arg("error") = std::nullopt)
      .def_readonly("lifecycle_transaction_id", &DisableHandoffStatusDTO::lifecycle_transaction_id)
      .def_readonly("lifecycle_transaction_generation",
                    &DisableHandoffStatusDTO::lifecycle_transaction_generation)
      .def_readonly("controllable_component_id",
                    &DisableHandoffStatusDTO::controllable_component_id)
      .def_readonly("scope_resolution", &DisableHandoffStatusDTO::scope_resolution)
      .def_readonly("component_scope_digest", &DisableHandoffStatusDTO::component_scope_digest)
      .def_readonly("configuration_fence_id", &DisableHandoffStatusDTO::configuration_fence_id)
      .def_readonly("configuration_fence_generation",
                    &DisableHandoffStatusDTO::configuration_fence_generation)
      .def_readonly("state", &DisableHandoffStatusDTO::state)
      .def_readonly("scope", &DisableHandoffStatusDTO::scope)
      .def_readonly("configuration_fence_handle",
                    &DisableHandoffStatusDTO::configuration_fence_handle)
      .def_readonly("stop_certificate", &DisableHandoffStatusDTO::stop_certificate)
      .def_property_readonly("cancelled_queued_operations",
                             [](const DisableHandoffStatusDTO& value) {
                               return immutable_snapshot(value.cancelled_queued_operations);
                             })
      .def_readonly("error", &DisableHandoffStatusDTO::error)
      .def("locally_valid", &DisableHandoffStatusDTO::locally_valid)
      .def(py::self == py::self)
      .def(py::self != py::self);

  py::class_<DisableHandoffAttemptReconciliationDTO>(
      module, "DisableHandoffAttemptReconciliationDTO", py::is_final())
      .def(py::init<DisableHandoffRequestDTO, DisableHandoffAttemptDispositionDTO,
                    std::optional<DisableHandoffAcceptedDTO>,
                    std::optional<DisableHandoffStatusDTO>, std::optional<ErrorDTO>>(),
           py::arg("request"), py::arg("disposition"), py::arg("acceptance") = std::nullopt,
           py::arg("status") = std::nullopt, py::arg("error") = std::nullopt)
      .def_readonly("request", &DisableHandoffAttemptReconciliationDTO::request)
      .def_readonly("disposition", &DisableHandoffAttemptReconciliationDTO::disposition)
      .def_readonly("acceptance", &DisableHandoffAttemptReconciliationDTO::acceptance)
      .def_readonly("status", &DisableHandoffAttemptReconciliationDTO::status)
      .def_readonly("error", &DisableHandoffAttemptReconciliationDTO::error)
      .def("locally_valid", &DisableHandoffAttemptReconciliationDTO::locally_valid)
      .def(py::self == py::self)
      .def(py::self != py::self);

  py::class_<DisableFenceReleaseRequestDTO>(module, "DisableFenceReleaseRequestDTO", py::is_final())
      .def(
          py::init<PublicRequestContextDTO, RequestId, LifecycleTransactionId,
                   LifecycleTransactionGeneration, ControllableComponentId, DisableHandoffStatusDTO,
                   std::uint64_t, ContentDigest, DisableHandoffComponentScopeExpectationDTO,
                   DisableHandoffAdmissionSnapshotDTO>(),
          py::arg("owner_request_context"), py::arg("initiating_public_request_id"),
          py::arg("lifecycle_transaction_id"), py::arg("lifecycle_transaction_generation"),
          py::arg("controllable_component_id"), py::arg("ready_handoff"),
          py::arg("release_precondition_status_sequence"),
          py::arg("release_precondition_status_digest"), py::arg("current_component_expectation"),
          py::arg("current_admission_snapshot"))
      .def_readonly("owner_request_context", &DisableFenceReleaseRequestDTO::owner_request_context)
      .def_readonly("initiating_public_request_id",
                    &DisableFenceReleaseRequestDTO::initiating_public_request_id)
      .def_readonly("lifecycle_transaction_id",
                    &DisableFenceReleaseRequestDTO::lifecycle_transaction_id)
      .def_readonly("lifecycle_transaction_generation",
                    &DisableFenceReleaseRequestDTO::lifecycle_transaction_generation)
      .def_readonly("controllable_component_id",
                    &DisableFenceReleaseRequestDTO::controllable_component_id)
      .def_readonly("ready_handoff", &DisableFenceReleaseRequestDTO::ready_handoff)
      .def_readonly("release_precondition_status_sequence",
                    &DisableFenceReleaseRequestDTO::release_precondition_status_sequence)
      .def_readonly("release_precondition_status_digest",
                    &DisableFenceReleaseRequestDTO::release_precondition_status_digest)
      .def_readonly("current_component_expectation",
                    &DisableFenceReleaseRequestDTO::current_component_expectation)
      .def_readonly("current_admission_snapshot",
                    &DisableFenceReleaseRequestDTO::current_admission_snapshot)
      .def("locally_valid", &DisableFenceReleaseRequestDTO::locally_valid)
      .def(py::self == py::self)
      .def(py::self != py::self);

  py::class_<DisableFenceReleasedDTO>(module, "DisableFenceReleasedDTO", py::is_final())
      .def(py::init<DisableFenceReleaseRequestDTO, RequestReplayStatusDTO, std::int64_t,
                    ConfigurationFenceId, ConfigurationFenceGeneration, ContentDigest,
                    ContentDigest>(),
           py::arg("request"), py::arg("replay_status"), py::arg("released_at_ns"),
           py::arg("configuration_fence_id"), py::arg("configuration_fence_generation"),
           py::arg("scope_digest"), py::arg("receipt_digest"))
      .def_readonly("request", &DisableFenceReleasedDTO::request)
      .def_readonly("replay_status", &DisableFenceReleasedDTO::replay_status)
      .def_readonly("released_at_ns", &DisableFenceReleasedDTO::released_at_ns)
      .def_readonly("configuration_fence_id", &DisableFenceReleasedDTO::configuration_fence_id)
      .def_readonly("configuration_fence_generation",
                    &DisableFenceReleasedDTO::configuration_fence_generation)
      .def_readonly("scope_digest", &DisableFenceReleasedDTO::scope_digest)
      .def_readonly("receipt_digest", &DisableFenceReleasedDTO::receipt_digest)
      .def("locally_valid", &DisableFenceReleasedDTO::locally_valid)
      .def("matches", &DisableFenceReleasedDTO::matches)
      .def(py::self == py::self)
      .def(py::self != py::self);

  py::class_<DisableFenceReleaseAttemptReconciliationDTO>(
      module, "DisableFenceReleaseAttemptReconciliationDTO", py::is_final())
      .def(py::init<DisableFenceReleaseRequestDTO, DisableFenceReleaseAttemptDispositionDTO,
                    std::optional<DisableFenceReleasedDTO>, std::optional<DisableHandoffStatusDTO>,
                    std::optional<ErrorDTO>>(),
           py::arg("request"), py::arg("disposition"), py::arg("release_receipt") = std::nullopt,
           py::arg("current_handoff_status") = std::nullopt, py::arg("error") = std::nullopt)
      .def_readonly("request", &DisableFenceReleaseAttemptReconciliationDTO::request)
      .def_readonly("disposition", &DisableFenceReleaseAttemptReconciliationDTO::disposition)
      .def_readonly("release_receipt",
                    &DisableFenceReleaseAttemptReconciliationDTO::release_receipt)
      .def_readonly("current_handoff_status",
                    &DisableFenceReleaseAttemptReconciliationDTO::current_handoff_status)
      .def_readonly("error", &DisableFenceReleaseAttemptReconciliationDTO::error)
      .def("locally_valid", &DisableFenceReleaseAttemptReconciliationDTO::locally_valid)
      .def(py::self == py::self)
      .def(py::self != py::self);

  py::class_<ComponentLifecycleDomainStatusDTO>(module, "ComponentLifecycleDomainStatusDTO",
                                                py::is_final())
      .def(py::init<ControlDomainId, std::optional<ExpectedControlDomainRevisionDTO>,
                    std::optional<ResolvedLifecycleControlDomainScopeDTO>,
                    std::optional<ResolvedLifecycleProviderOccurrenceDTO>,
                    std::optional<ComponentLifecycleStateDTO>,
                    std::optional<ComponentLifecycleStateDTO>, ComponentLifecycleRequestedStateDTO,
                    ComponentLifecycleDomainActionStateDTO, ComponentLifecycleDomainEffectDTO,
                    std::vector<BrandLifecyclePrimitiveResultDTO>,
                    std::optional<LifecycleReconciliationEvidenceDTO>, std::optional<ErrorDTO>>(),
           py::arg("control_domain_id"), py::arg("expected_domain"), py::arg("current_scope"),
           py::arg("resolved_provider_occurrence"), py::arg("previous_state"),
           py::arg("current_state"), py::arg("requested_state"), py::arg("action_state"),
           py::arg("effect"), py::arg("primitive_attempts"), py::arg("reconciliation_evidence"),
           py::arg("error"))
      .def_readonly("control_domain_id", &ComponentLifecycleDomainStatusDTO::control_domain_id)
      .def_readonly("expected_domain", &ComponentLifecycleDomainStatusDTO::expected_domain)
      .def_readonly("current_scope", &ComponentLifecycleDomainStatusDTO::current_scope)
      .def_readonly("resolved_provider_occurrence",
                    &ComponentLifecycleDomainStatusDTO::resolved_provider_occurrence)
      .def_readonly("previous_state", &ComponentLifecycleDomainStatusDTO::previous_state)
      .def_readonly("current_state", &ComponentLifecycleDomainStatusDTO::current_state)
      .def_readonly("requested_state", &ComponentLifecycleDomainStatusDTO::requested_state)
      .def_readonly("action_state", &ComponentLifecycleDomainStatusDTO::action_state)
      .def_readonly("effect", &ComponentLifecycleDomainStatusDTO::effect)
      .def_property_readonly("primitive_attempts",
                             [](const ComponentLifecycleDomainStatusDTO& value) {
                               return immutable_snapshot(value.primitive_attempts);
                             })
      .def_readonly("reconciliation_evidence",
                    &ComponentLifecycleDomainStatusDTO::reconciliation_evidence)
      .def_readonly("error", &ComponentLifecycleDomainStatusDTO::error)
      .def("locally_valid", &ComponentLifecycleDomainStatusDTO::locally_valid)
      .def(py::self == py::self)
      .def(py::self != py::self);

  py::class_<ComponentLifecycleStatusDTO>(module, "ComponentLifecycleStatusDTO", py::is_final())
      .def(py::init<ComponentLifecycleAcceptedDTO, ComponentLifecycleIntentDTO, std::uint64_t,
                    RequestOwnerEpoch, std::optional<ComponentLifecycleStateDTO>,
                    std::optional<ComponentLifecycleStateDTO>, ComponentLifecycleRequestedStateDTO,
                    ComponentLifecycleTransactionPhaseDTO,
                    std::vector<ComponentLifecycleDomainStatusDTO>,
                    std::optional<ConfigurationFenceHandleDTO>,
                    std::optional<DisableHandoffStatusDTO>, std::optional<DisableFenceReleasedDTO>,
                    bool, std::optional<ComponentLifecycleTerminalResultDTO>,
                    std::optional<ErrorDTO>, std::int64_t, std::int64_t, std::int64_t>(),
           py::arg("acceptance"), py::arg("accepted_intent"), py::arg("status_sequence"),
           py::arg("status_owner_epoch"), py::arg("previous_state"), py::arg("current_state"),
           py::arg("requested_state"), py::arg("phase"), py::arg("domains"),
           py::arg("configuration_fence"), py::arg("disable_handoff"),
           py::arg("disable_fence_released"), py::arg("reconciliation_required"),
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
      .def_readonly("configuration_fence", &ComponentLifecycleStatusDTO::configuration_fence)
      .def_readonly("disable_handoff", &ComponentLifecycleStatusDTO::disable_handoff)
      .def_readonly("disable_fence_released", &ComponentLifecycleStatusDTO::disable_fence_released)
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
