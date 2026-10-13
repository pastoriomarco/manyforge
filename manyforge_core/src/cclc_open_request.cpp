#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <limits>
#include <manyforge_content_identity/canonical_value.hpp>
#include <manyforge_core/cclc_open_request.hpp>
#include <stdexcept>
#include <string>
#include <utility>

namespace manyforge::core {
namespace {

using content_identity::CanonicalValue;

constexpr std::uint64_t kMaximumSafeUnsignedInteger = 9'007'199'254'740'991ULL;
constexpr std::int64_t kMaximumSafeSignedInteger = 9'007'199'254'740'991LL;

class DecodeFailure final : public std::invalid_argument {
 public:
  using std::invalid_argument::invalid_argument;
};

[[noreturn]] void fail(std::string message) {
  throw DecodeFailure(std::move(message));
}

[[nodiscard]] bool contains_name(const std::initializer_list<std::string_view> names,
                                 const std::string_view candidate) {
  return std::find(names.begin(), names.end(), candidate) != names.end();
}

void exact_object(const CanonicalValue& value,
                  const std::initializer_list<std::string_view> required,
                  const std::initializer_list<std::string_view> optional,
                  const std::string_view context) {
  if (!value.is_object()) {
    fail(std::string(context) + " is not an object");
  }
  for (const auto name : required) {
    if (value.find_object_member(name) == nullptr) {
      fail(std::string(context) + " is missing " + std::string(name));
    }
  }
  for (const auto& [name, member_value] : value.as_object()) {
    static_cast<void>(member_value);
    if (!contains_name(required, name) && !contains_name(optional, name)) {
      fail(std::string(context) + " contains unknown member " + name);
    }
  }
}

[[nodiscard]] const CanonicalValue& member(const CanonicalValue& object,
                                           const std::string_view name,
                                           const std::string_view context) {
  const auto* value = object.find_object_member(name);
  if (value == nullptr) {
    fail(std::string(context) + " is missing " + std::string(name));
  }
  return *value;
}

[[nodiscard]] const std::string& text(const CanonicalValue& value, const std::string_view context) {
  if (!value.is_string()) {
    fail(std::string(context) + " is not a string");
  }
  return value.as_string();
}

[[nodiscard]] const CanonicalValue::Array& bounded_array(const CanonicalValue& value,
                                                         const std::size_t maximum,
                                                         const std::string_view context) {
  if (!value.is_array() || value.as_array().size() > maximum) {
    fail(std::string(context) + " exceeds its array bound");
  }
  return value.as_array();
}

[[nodiscard]] std::uint64_t unsigned_integer(const CanonicalValue& value,
                                             const std::string_view context) {
  if (!value.is_number()) {
    fail(std::string(context) + " is not a number");
  }
  const double number = value.as_number();
  if (number < 0.0 || number > static_cast<double>(kMaximumSafeUnsignedInteger) ||
      std::trunc(number) != number) {
    fail(std::string(context) + " is not an I-JSON-safe unsigned integer");
  }
  return static_cast<std::uint64_t>(number);
}

template <typename Enum>
[[nodiscard]] Enum enum_value(
    const CanonicalValue& value,
    const std::initializer_list<std::pair<std::string_view, Enum>> alternatives,
    const std::string_view context) {
  const auto& spelling = text(value, context);
  for (const auto& [candidate, result] : alternatives) {
    if (candidate == spelling) {
      return result;
    }
  }
  fail(std::string(context) + " has an unknown enum spelling");
}

[[nodiscard]] CanonicalValue safe_unsigned(const std::uint64_t value) {
  if (value > kMaximumSafeUnsignedInteger) {
    throw std::invalid_argument("unsigned value exceeds I-JSON safe integer range");
  }
  return CanonicalValue(static_cast<double>(value));
}

[[nodiscard]] CanonicalValue safe_signed(const std::int64_t value) {
  if (value < -kMaximumSafeSignedInteger || value > kMaximumSafeSignedInteger) {
    throw std::invalid_argument("signed value exceeds I-JSON safe integer range");
  }
  return CanonicalValue(static_cast<double>(value));
}

template <typename StrongId>
[[nodiscard]] CanonicalValue nullable_identity(const std::optional<StrongId>& value) {
  return value.has_value() ? CanonicalValue(value->value()) : CanonicalValue(nullptr);
}

template <typename StrongCounter>
[[nodiscard]] CanonicalValue nullable_counter(const std::optional<StrongCounter>& value) {
  return value.has_value() ? safe_unsigned(value->value()) : CanonicalValue(nullptr);
}

[[nodiscard]] PublicRequestContextDTO request_context(const CanonicalValue& value) {
  exact_object(value, {"request_id", "request_owner_epoch", "source_label"}, {"correlation_id"},
               "request context");
  std::optional<std::string> correlation;
  if (const auto* candidate = value.find_object_member("correlation_id"); candidate != nullptr) {
    correlation = text(*candidate, "correlation_id");
  }
  return {RequestId(text(member(value, "request_id", "request context"), "request_id")),
          RequestOwnerEpoch(
              text(member(value, "request_owner_epoch", "request context"), "request_owner_epoch")),
          text(member(value, "source_label", "request context"), "source_label"),
          std::move(correlation)};
}

[[nodiscard]] ExpectedActiveControlConfigurationDTO expected_active_configuration(
    const CanonicalValue& value) {
  exact_object(value, {"presence"},
               {"active_control_configuration_id", "active_control_configuration_generation"},
               "expected active configuration");
  const auto presence = enum_value<ExpectedActiveControlConfigurationPresenceDTO>(
      member(value, "presence", "expected active configuration"),
      {{"absent", ExpectedActiveControlConfigurationPresenceDTO::kAbsent},
       {"present", ExpectedActiveControlConfigurationPresenceDTO::kPresent}},
      "expected active configuration presence");
  std::optional<ActiveControlConfigurationId> identifier;
  std::optional<ActiveControlConfigurationGeneration> generation;
  if (const auto* candidate = value.find_object_member("active_control_configuration_id");
      candidate != nullptr) {
    identifier = ActiveControlConfigurationId(text(*candidate, "active configuration ID"));
  }
  if (const auto* candidate = value.find_object_member("active_control_configuration_generation");
      candidate != nullptr) {
    generation = ActiveControlConfigurationGeneration(
        unsigned_integer(*candidate, "active configuration generation"));
  }
  ExpectedActiveControlConfigurationDTO result{presence, std::move(identifier), generation};
  if (!result.locally_valid()) {
    fail("expected active configuration is inconsistent");
  }
  return result;
}

[[nodiscard]] ExpectedControlBindingRevisionDTO expected_control_binding(
    const CanonicalValue& value) {
  exact_object(value, {"control_binding_id", "control_binding_revision", "control_route_id"}, {},
               "expected control binding");
  ExpectedControlBindingRevisionDTO result{
      ControlBindingId(text(member(value, "control_binding_id", "expected control binding"),
                            "control_binding_id")),
      ControlBindingRevision(
          text(member(value, "control_binding_revision", "expected control binding"),
               "control_binding_revision")),
      ControlRouteId(
          text(member(value, "control_route_id", "expected control binding"), "control_route_id"))};
  if (!result.locally_valid()) {
    fail("expected control binding is invalid");
  }
  return result;
}

[[nodiscard]] ExpectedControlDomainRevisionDTO expected_control_domain(
    const CanonicalValue& value) {
  exact_object(
      value,
      {"control_domain_id", "control_domain_revision", "controller_topology_id",
       "topology_revision", "expected_active_control_configuration", "expected_control_bindings"},
      {}, "expected control domain");
  std::vector<ExpectedControlBindingRevisionDTO> bindings;
  const auto& encoded =
      bounded_array(member(value, "expected_control_bindings", "expected control domain"),
                    kMaxLifecycleExpectedBindings, "expected control bindings");
  bindings.reserve(encoded.size());
  for (const auto& row : encoded) {
    bindings.push_back(expected_control_binding(row));
  }
  ExpectedControlDomainRevisionDTO result{
      ControlDomainId(
          text(member(value, "control_domain_id", "expected control domain"), "control_domain_id")),
      ControlDomainRevision(
          text(member(value, "control_domain_revision", "expected control domain"),
               "control_domain_revision")),
      ControllerTopologyId(text(member(value, "controller_topology_id", "expected control domain"),
                                "controller_topology_id")),
      TopologyRevision(
          text(member(value, "topology_revision", "expected control domain"), "topology_revision")),
      expected_active_configuration(
          member(value, "expected_active_control_configuration", "expected control domain")),
      std::move(bindings)};
  if (!result.locally_valid()) {
    fail("expected control domain is invalid");
  }
  return result;
}

[[nodiscard]] std::optional<ProgramNodeAdmissionEvidenceDTO> program_node_evidence(
    const CanonicalValue& object) {
  const auto* value = object.find_object_member("program_node_evidence");
  if (!value) return std::nullopt;
  exact_object(
      *value,
      {"program_runner_owner_epoch", "program_run_id", "parent_program_cycle_claim_generation",
       "node_instance_id", "node_attempt_id", "owner_api"},
      {}, "Program node evidence");
  const auto field = [&](std::string_view name) -> const CanonicalValue& {
    return member(*value, name, "Program node evidence");
  };
  ProgramNodeAdmissionEvidenceDTO result{
      RequestOwnerEpoch(text(field("program_runner_owner_epoch"), "Program owner")),
      ProgramRunId(text(field("program_run_id"), "Program run")),
      CellAdmissionClaimGeneration(
          unsigned_integer(field("parent_program_cycle_claim_generation"), "Program parent claim")),
      text(field("node_instance_id"), "node instance"),
      text(field("node_attempt_id"), "node attempt"),
      text(field("owner_api"), "owner API")};
  if (!result.locally_valid()) fail("Program node admission evidence is invalid");
  return result;
}

[[nodiscard]] ComponentLifecycleIntentDTO lifecycle_intent(const CanonicalValue& value,
                                                           RequestAttributionDTO attribution) {
  exact_object(value,
               {"request_context", "controllable_component_id", "intent", "expected_graph_revision",
                "expected_component_lifecycle_generation", "expected_control_domains",
                "expected_admission_generation"},
               {"reason", "program_node_evidence"}, "component lifecycle intent");
  std::vector<ExpectedControlDomainRevisionDTO> domains;
  const auto& encoded =
      bounded_array(member(value, "expected_control_domains", "component lifecycle intent"),
                    kMaxLifecycleExpectedDomains, "expected control domains");
  domains.reserve(encoded.size());
  for (const auto& row : encoded) {
    domains.push_back(expected_control_domain(row));
  }
  std::optional<std::string> reason;
  if (const auto* candidate = value.find_object_member("reason"); candidate != nullptr) {
    reason = text(*candidate, "lifecycle reason");
  }
  ComponentLifecycleIntentDTO result{
      request_context(member(value, "request_context", "component lifecycle intent")),
      ControllableComponentId(
          text(member(value, "controllable_component_id", "component lifecycle intent"),
               "controllable_component_id")),
      enum_value<ComponentLifecycleIntentKindDTO>(
          member(value, "intent", "component lifecycle intent"),
          {{"prepare", ComponentLifecycleIntentKindDTO::kPrepare},
           {"enable", ComponentLifecycleIntentKindDTO::kEnable},
           {"disable", ComponentLifecycleIntentKindDTO::kDisable},
           {"recover", ComponentLifecycleIntentKindDTO::kRecover}},
          "lifecycle intent"),
      GraphRevision(text(member(value, "expected_graph_revision", "component lifecycle intent"),
                         "expected_graph_revision")),
      ComponentLifecycleGeneration(unsigned_integer(
          member(value, "expected_component_lifecycle_generation", "component lifecycle intent"),
          "expected component lifecycle generation")),
      std::move(domains),
      std::move(reason),
      std::move(attribution),
      AdmissionGeneration(unsigned_integer(
          member(value, "expected_admission_generation", "component lifecycle intent"),
          "expected admission generation")),
      program_node_evidence(value)};
  if (!result.locally_valid()) {
    fail("component lifecycle intent is invalid");
  }
  return result;
}

[[nodiscard]] DesiredControlBindingRouteDTO desired_binding_route(const CanonicalValue& value) {
  exact_object(value, {"control_binding_id", "control_binding_revision", "control_route_id"}, {},
               "desired control binding route");
  return {
      ControlBindingId(
          text(member(value, "control_binding_id", "desired route"), "control_binding_id")),
      ControlBindingRevision(text(member(value, "control_binding_revision", "desired route"),
                                  "control_binding_revision")),
      ControlRouteId(text(member(value, "control_route_id", "desired route"), "control_route_id"))};
}

[[nodiscard]] DesiredControlConfigurationDTO desired_configuration(const CanonicalValue& value) {
  exact_object(value,
               {"control_domain_id", "control_domain_revision", "controller_topology_id",
                "topology_revision", "activation_set_id", "binding_routes"},
               {}, "desired control configuration");
  std::vector<DesiredControlBindingRouteDTO> routes;
  const auto& encoded = bounded_array(member(value, "binding_routes", "desired configuration"),
                                      kMaxRouteSelectionAffectedBindings, "desired binding routes");
  routes.reserve(encoded.size());
  for (const auto& row : encoded) {
    routes.push_back(desired_binding_route(row));
  }
  return {
      ControlDomainId(
          text(member(value, "control_domain_id", "desired configuration"), "control_domain_id")),
      ControlDomainRevision(text(member(value, "control_domain_revision", "desired configuration"),
                                 "control_domain_revision")),
      ControllerTopologyId(text(member(value, "controller_topology_id", "desired configuration"),
                                "controller_topology_id")),
      TopologyRevision(
          text(member(value, "topology_revision", "desired configuration"), "topology_revision")),
      ActivationSetId(
          text(member(value, "activation_set_id", "desired configuration"), "activation_set_id")),
      std::move(routes)};
}

[[nodiscard]] RouteSelectionComponentExpectationDTO component_expectation(
    const CanonicalValue& value) {
  exact_object(value,
               {"controllable_component_id", "expected_component_lifecycle_generation",
                "expected_component_lifecycle_state"},
               {}, "route component expectation");
  RouteSelectionComponentExpectationDTO result{
      ControllableComponentId(
          text(member(value, "controllable_component_id", "route component expectation"),
               "controllable_component_id")),
      ComponentLifecycleGeneration(unsigned_integer(
          member(value, "expected_component_lifecycle_generation", "route component expectation"),
          "expected component lifecycle generation")),
      enum_value<ComponentLifecycleStateDTO>(
          member(value, "expected_component_lifecycle_state", "route component expectation"),
          {{"undeclared", ComponentLifecycleStateDTO::kUndeclared},
           {"declared", ComponentLifecycleStateDTO::kDeclared},
           {"prepared", ComponentLifecycleStateDTO::kPrepared},
           {"active", ComponentLifecycleStateDTO::kActive},
           {"degraded", ComponentLifecycleStateDTO::kDegraded},
           {"faulted", ComponentLifecycleStateDTO::kFaulted},
           {"disabling", ComponentLifecycleStateDTO::kDisabling},
           {"disabled", ComponentLifecycleStateDTO::kDisabled}},
          "expected component lifecycle state")};
  if (!result.locally_valid()) {
    fail("route component expectation is invalid");
  }
  return result;
}

[[nodiscard]] RouteSelectionIntentDTO route_intent(const CanonicalValue& value,
                                                   RequestAttributionDTO attribution) {
  exact_object(
      value,
      {"request_context", "expected_retained_bundle_id", "expected_graph_revision",
       "desired_configuration", "expected_active_control_configuration", "component_expectations"},
      {"expected_admission_generation", "program_node_evidence"}, "route selection intent");
  std::vector<RouteSelectionComponentExpectationDTO> components;
  const auto& encoded =
      bounded_array(member(value, "component_expectations", "route selection intent"),
                    kMaxRouteSelectionComponents, "route component expectations");
  components.reserve(encoded.size());
  for (const auto& row : encoded) {
    components.push_back(component_expectation(row));
  }
  RouteSelectionIntentDTO result{
      request_context(member(value, "request_context", "route selection intent")),
      RetainedBundleId(text(member(value, "expected_retained_bundle_id", "route selection intent"),
                            "expected_retained_bundle_id")),
      GraphRevision(text(member(value, "expected_graph_revision", "route selection intent"),
                         "expected_graph_revision")),
      desired_configuration(member(value, "desired_configuration", "route selection intent")),
      expected_active_configuration(
          member(value, "expected_active_control_configuration", "route selection intent")),
      std::move(components),
      std::move(attribution)};
  if (const auto* generation = value.find_object_member("expected_admission_generation")) {
    result.expected_admission_generation =
        AdmissionGeneration(unsigned_integer(*generation, "expected admission generation"));
  }
  result.program_node_evidence = program_node_evidence(value);
  if (!result.locally_valid()) {
    fail("route selection intent is invalid");
  }
  return result;
}

[[nodiscard]] CanonicalValue error_value(const ErrorDTO& error) {
  CanonicalValue::Object details;
  details.reserve(error.details.size());
  for (const auto& [key, value] : error.details) {
    details.emplace_back(key, CanonicalValue(value));
  }
  return CanonicalValue(CanonicalValue::Object{{"code", CanonicalValue(error.code)},
                                               {"details", CanonicalValue(std::move(details))},
                                               {"message", CanonicalValue(error.message)}});
}

[[nodiscard]] CanonicalValue expected_active_value(
    const ExpectedActiveControlConfigurationDTO& value) {
  CanonicalValue::Object result;
  result.reserve(value.presence == ExpectedActiveControlConfigurationPresenceDTO::kPresent ? 3U
                                                                                           : 1U);
  if (value.presence == ExpectedActiveControlConfigurationPresenceDTO::kPresent) {
    result.emplace_back("active_control_configuration_generation",
                        safe_unsigned(value.active_control_configuration_generation->value()));
    result.emplace_back("active_control_configuration_id",
                        CanonicalValue(value.active_control_configuration_id->value()));
  }
  result.emplace_back(
      "presence",
      CanonicalValue(value.presence == ExpectedActiveControlConfigurationPresenceDTO::kPresent
                         ? "present"
                         : "absent"));
  return CanonicalValue(std::move(result));
}

[[nodiscard]] CanonicalValue desired_configuration_value(
    const DesiredControlConfigurationDTO& value) {
  CanonicalValue::Array routes;
  routes.reserve(value.binding_routes.size());
  for (const auto& route : value.binding_routes) {
    routes.emplace_back(CanonicalValue::Object{
        {"control_binding_id", CanonicalValue(route.control_binding_id.value())},
        {"control_binding_revision", CanonicalValue(route.control_binding_revision.value())},
        {"control_route_id", CanonicalValue(route.control_route_id.value())}});
  }
  return CanonicalValue(CanonicalValue::Object{
      {"activation_set_id", CanonicalValue(value.activation_set_id.value())},
      {"binding_routes", CanonicalValue(std::move(routes))},
      {"control_domain_id", CanonicalValue(value.control_domain_id.value())},
      {"control_domain_revision", CanonicalValue(value.control_domain_revision.value())},
      {"controller_topology_id", CanonicalValue(value.controller_topology_id.value())},
      {"topology_revision", CanonicalValue(value.topology_revision.value())}});
}

[[nodiscard]] CanonicalValue route_template_set_value(
    const CCLCRouteSelectionTemplateSetDTO& value) {
  CanonicalValue::Array templates;
  templates.reserve(value.templates.size());
  for (const auto& row : value.templates) {
    CanonicalValue::Array components;
    components.reserve(row.component_expectations.size());
    for (const auto& component : row.component_expectations) {
      components.emplace_back(CanonicalValue::Object{
          {"controllable_component_id",
           CanonicalValue(component.controllable_component_id.value())},
          {"expected_component_lifecycle_generation",
           safe_unsigned(component.expected_component_lifecycle_generation.value())},
          {"expected_component_lifecycle_state",
           CanonicalValue(std::string(to_string(component.expected_component_lifecycle_state)))}});
    }
    templates.emplace_back(CanonicalValue::Object{
        {"component_expectations", CanonicalValue(std::move(components))},
        {"desired_configuration", desired_configuration_value(row.desired_configuration)},
        {"expected_active_control_configuration",
         expected_active_value(row.expected_active_control_configuration)}});
  }
  return CanonicalValue(CanonicalValue::Object{
      {"controllable_component_id", CanonicalValue(value.controllable_component_id.value())},
      {"graph_revision", CanonicalValue(value.graph_revision.value())},
      {"retained_bundle_id", CanonicalValue(value.retained_bundle_id.value())},
      {"templates", CanonicalValue(std::move(templates))}});
}

[[nodiscard]] std::string_view lifecycle_phase(
    const ComponentLifecycleTransactionPhaseDTO value) noexcept {
  switch (value) {
    case ComponentLifecycleTransactionPhaseDTO::kAccepted:
      return "accepted";
    case ComponentLifecycleTransactionPhaseDTO::kAcquiringDomains:
      return "acquiring_domains";
    case ComponentLifecycleTransactionPhaseDTO::kContaining:
      return "containing";
    case ComponentLifecycleTransactionPhaseDTO::kExecuting:
      return "executing";
    case ComponentLifecycleTransactionPhaseDTO::kVerifying:
      return "verifying";
    case ComponentLifecycleTransactionPhaseDTO::kOutcomeUnknown:
      return "outcome_unknown";
    case ComponentLifecycleTransactionPhaseDTO::kReconciling:
      return "reconciling";
    case ComponentLifecycleTransactionPhaseDTO::kSucceeded:
      return "succeeded";
    case ComponentLifecycleTransactionPhaseDTO::kFailed:
      return "failed";
  }
  return {};
}

[[nodiscard]] std::string_view lifecycle_action_state(
    const ComponentLifecycleDomainActionStateDTO value) noexcept {
  switch (value) {
    case ComponentLifecycleDomainActionStateDTO::kReserved:
      return "reserved";
    case ComponentLifecycleDomainActionStateDTO::kNotStarted:
      return "not_started";
    case ComponentLifecycleDomainActionStateDTO::kExecuting:
      return "executing";
    case ComponentLifecycleDomainActionStateDTO::kVerifying:
      return "verifying";
    case ComponentLifecycleDomainActionStateDTO::kReconciling:
      return "reconciling";
    case ComponentLifecycleDomainActionStateDTO::kSucceeded:
      return "succeeded";
    case ComponentLifecycleDomainActionStateDTO::kFailed:
      return "failed";
    case ComponentLifecycleDomainActionStateDTO::kOutcomeUnknown:
      return "outcome_unknown";
  }
  return {};
}

[[nodiscard]] std::string_view lifecycle_effect(
    const ComponentLifecycleDomainEffectDTO value) noexcept {
  switch (value) {
    case ComponentLifecycleDomainEffectDTO::kNone:
      return "none";
    case ComponentLifecycleDomainEffectDTO::kKnownNoEffect:
      return "known_no_effect";
    case ComponentLifecycleDomainEffectDTO::kEffectObserved:
      return "effect_observed";
    case ComponentLifecycleDomainEffectDTO::kPartialEffectObserved:
      return "partial_effect_observed";
    case ComponentLifecycleDomainEffectDTO::kEffectUncertain:
      return "effect_uncertain";
  }
  return {};
}

[[nodiscard]] std::string_view lifecycle_terminal(
    const ComponentLifecycleTerminalResultDTO value) noexcept {
  switch (value) {
    case ComponentLifecycleTerminalResultDTO::kSucceeded:
      return "succeeded";
    case ComponentLifecycleTerminalResultDTO::kFailedKnownNoEffect:
      return "failed_known_no_effect";
    case ComponentLifecycleTerminalResultDTO::kFailedPartialEffect:
      return "failed_partial_effect";
  }
  return {};
}

[[nodiscard]] std::string_view route_phase(const RouteSelectionTransactionPhaseDTO value) noexcept {
  switch (value) {
    case RouteSelectionTransactionPhaseDTO::kAccepted:
      return "accepted";
    case RouteSelectionTransactionPhaseDTO::kAcquiringFence:
      return "acquiring_fence";
    case RouteSelectionTransactionPhaseDTO::kObserving:
      return "observing";
    case RouteSelectionTransactionPhaseDTO::kDeactivating:
      return "deactivating";
    case RouteSelectionTransactionPhaseDTO::kActivating:
      return "activating";
    case RouteSelectionTransactionPhaseDTO::kVerifying:
      return "verifying";
    case RouteSelectionTransactionPhaseDTO::kPublishing:
      return "publishing";
    case RouteSelectionTransactionPhaseDTO::kReleasingFence:
      return "releasing_fence";
    case RouteSelectionTransactionPhaseDTO::kOutcomeUnknown:
      return "outcome_unknown";
    case RouteSelectionTransactionPhaseDTO::kReconciling:
      return "reconciling";
    case RouteSelectionTransactionPhaseDTO::kSucceeded:
      return "succeeded";
    case RouteSelectionTransactionPhaseDTO::kFailed:
      return "failed";
  }
  return {};
}

[[nodiscard]] std::string_view route_effect(const RouteSelectionEffectDTO value) noexcept {
  switch (value) {
    case RouteSelectionEffectDTO::kNone:
      return "none";
    case RouteSelectionEffectDTO::kKnownNoEffect:
      return "known_no_effect";
    case RouteSelectionEffectDTO::kEffectObserved:
      return "effect_observed";
    case RouteSelectionEffectDTO::kPartialEffectObserved:
      return "partial_effect_observed";
    case RouteSelectionEffectDTO::kEffectUncertain:
      return "effect_uncertain";
  }
  return {};
}

[[nodiscard]] std::string_view route_terminal(
    const RouteSelectionTerminalResultDTO value) noexcept {
  switch (value) {
    case RouteSelectionTerminalResultDTO::kSucceeded:
      return "succeeded";
    case RouteSelectionTerminalResultDTO::kFailedKnownNoEffect:
      return "failed_known_no_effect";
    case RouteSelectionTerminalResultDTO::kFailedPartialEffect:
      return "failed_partial_effect";
  }
  return {};
}

[[nodiscard]] std::string_view active_configuration_state(
    const ActiveControlConfigurationStateDTO value) noexcept {
  switch (value) {
    case ActiveControlConfigurationStateDTO::kReconciled:
      return "reconciled";
    case ActiveControlConfigurationStateDTO::kExternalOrUnknownUnreconciled:
      return "external_or_unknown_unreconciled";
  }
  return {};
}

[[nodiscard]] CanonicalValue active_configuration_value(
    const ActiveControlConfigurationDTO& value) {
  CanonicalValue::Array routes;
  routes.reserve(value.binding_routes.size());
  for (const auto& row : value.binding_routes) {
    routes.emplace_back(CanonicalValue::Object{
        {"control_binding_id", CanonicalValue(row.control_binding_id.value())},
        {"control_binding_revision", CanonicalValue(row.control_binding_revision.value())},
        {"control_route_id", CanonicalValue(row.control_route_id.value())}});
  }
  return CanonicalValue(CanonicalValue::Object{
      {"activation_set_id", CanonicalValue(value.activation_set_id.value())},
      {"active_control_configuration_generation",
       safe_unsigned(value.active_control_configuration_generation.value())},
      {"active_control_configuration_id",
       CanonicalValue(value.active_control_configuration_id.value())},
      {"binding_routes", CanonicalValue(std::move(routes))},
      {"control_domain_id", CanonicalValue(value.control_domain_id.value())},
      {"control_domain_revision", CanonicalValue(value.control_domain_revision.value())},
      {"controller_topology_id", CanonicalValue(value.controller_topology_id.value())},
      {"source_runtime_epoch", CanonicalValue(value.source_runtime_epoch.value())},
      {"source_runtime_unit_id", CanonicalValue(value.source_runtime_unit_id.value())},
      {"source_sequence", safe_unsigned(value.source_sequence)},
      {"state", CanonicalValue(std::string(active_configuration_state(value.state)))},
      {"topology_revision", CanonicalValue(value.topology_revision.value())}});
}

[[nodiscard]] CanonicalValue lifecycle_acceptance_value(
    const ComponentLifecycleAcceptedDTO& value) {
  return CanonicalValue(CanonicalValue::Object{
      {"accepted_at_ns", content_identity::nonnegative_int64_value(value.accepted_at_ns)},
      {"component_lifecycle_generation",
       safe_unsigned(value.component_lifecycle_generation.value())},
      {"lifecycle_transaction_generation",
       safe_unsigned(value.lifecycle_transaction_generation.value())},
      {"lifecycle_transaction_id", CanonicalValue(value.lifecycle_transaction_id.value())},
      {"request_id", CanonicalValue(value.request_id.value())},
      {"timeout_duration_ns", safe_signed(value.timeout_duration_ns)}});
}

[[nodiscard]] CanonicalValue lifecycle_status_value(const ComponentLifecycleStatusDTO& value) {
  CanonicalValue::Array domains;
  domains.reserve(value.domains.size());
  for (const auto& domain : value.domains) {
    domains.emplace_back(CanonicalValue::Object{
        {"action_state", CanonicalValue(std::string(lifecycle_action_state(domain.action_state)))},
        {"control_domain_id", CanonicalValue(domain.control_domain_id.value())},
        {"current_state", domain.current_state.has_value()
                              ? CanonicalValue(std::string(to_string(*domain.current_state)))
                              : CanonicalValue(nullptr)},
        {"effect", CanonicalValue(std::string(lifecycle_effect(domain.effect)))},
        {"error",
         domain.error.has_value() ? error_value(*domain.error) : CanonicalValue(nullptr)}});
  }
  return CanonicalValue(CanonicalValue::Object{
      {"acceptance", lifecycle_acceptance_value(value.acceptance)},
      {"accepted_at_ns", content_identity::nonnegative_int64_value(value.accepted_at_ns)},
      {"controllable_component_id",
       CanonicalValue(value.accepted_intent.controllable_component_id.value())},
      {"current_state", value.current_state.has_value()
                            ? CanonicalValue(std::string(to_string(*value.current_state)))
                            : CanonicalValue(nullptr)},
      {"domains", CanonicalValue(std::move(domains))},
      {"error", value.error.has_value() ? error_value(*value.error) : CanonicalValue(nullptr)},
      {"latest_updated_at_ns", content_identity::nonnegative_int64_value(value.latest_updated_at_ns)},
      {"phase", CanonicalValue(std::string(lifecycle_phase(value.phase)))},
      {"previous_state", value.previous_state.has_value()
                             ? CanonicalValue(std::string(to_string(*value.previous_state)))
                             : CanonicalValue(nullptr)},
      {"reconciliation_required", CanonicalValue(value.reconciliation_required)},
      {"remaining_duration_ns", safe_signed(value.remaining_duration_ns)},
      {"status_owner_epoch", CanonicalValue(value.status_owner_epoch.value())},
      {"status_sequence", safe_unsigned(value.status_sequence)},
      {"terminal_result",
       value.terminal_result.has_value()
           ? CanonicalValue(std::string(lifecycle_terminal(*value.terminal_result)))
           : CanonicalValue(nullptr)}});
}

[[nodiscard]] CanonicalValue lifecycle_result_value(const CCLCLifecycleRequestResultDTO& value) {
  CanonicalValue::Object fields{
      {"acceptance", value.acceptance.has_value() ? lifecycle_acceptance_value(*value.acceptance)
                                                  : CanonicalValue(nullptr)},
      {"replay_status", CanonicalValue(std::string(to_string(value.replay_status)))},
      {"status",
       value.status.has_value() ? lifecycle_status_value(*value.status) : CanonicalValue(nullptr)}};
  if (value.rejection) fields.emplace_back("rejection", error_value(*value.rejection));
  // Present only for the authorization-only Recover of an open incident.
  if (value.incident_recovery) {
    const auto& recovery = *value.incident_recovery;
    fields.emplace_back(
        "incident_recovery",
        CanonicalValue(CanonicalValue::Object{
            {"acknowledged_at_ns",
             content_identity::nonnegative_int64_value(recovery.acknowledged_at_ns)},
            {"acknowledged_by", CanonicalValue(recovery.acknowledged_by)},
            {"cause_class", CanonicalValue(std::string(to_string(recovery.cause_class)))},
            {"controllable_component_id",
             CanonicalValue(recovery.controllable_component_id.value())},
            {"incident_id", CanonicalValue(recovery.incident_id)}}));
  }
  return CanonicalValue(std::move(fields));
}

[[nodiscard]] CanonicalValue route_acceptance_value(const RouteSelectionAcceptedDTO& value) {
  return CanonicalValue(CanonicalValue::Object{
      {"accepted_at_ns", content_identity::nonnegative_int64_value(value.accepted_at_ns)},
      {"candidate_active_control_configuration_generation",
       safe_unsigned(value.candidate_active_control_configuration_generation.value())},
      {"candidate_active_control_configuration_id",
       CanonicalValue(value.candidate_active_control_configuration_id.value())},
      {"control_domain_id", CanonicalValue(value.desired_configuration.control_domain_id.value())},
      {"request_id", CanonicalValue(value.request_id.value())},
      {"route_selection_transaction_generation",
       safe_unsigned(value.route_selection_transaction_generation.value())},
      {"route_selection_transaction_id",
       CanonicalValue(value.route_selection_transaction_id.value())},
      {"timeout_duration_ns", safe_signed(value.timeout_duration_ns)}});
}

[[nodiscard]] CanonicalValue route_status_value(const RouteSelectionStatusDTO& value) {
  CanonicalValue::Array bindings;
  bindings.reserve(value.affected_control_binding_ids.size());
  for (const auto& row : value.affected_control_binding_ids) {
    bindings.emplace_back(row.value());
  }
  CanonicalValue::Array controllers;
  controllers.reserve(value.affected_controller_role_ids.size());
  for (const auto& row : value.affected_controller_role_ids) {
    controllers.emplace_back(row.value());
  }
  return CanonicalValue(CanonicalValue::Object{
      {"acceptance", route_acceptance_value(value.acceptance)},
      {"accepted_at_ns", content_identity::nonnegative_int64_value(value.accepted_at_ns)},
      {"affected_control_binding_ids", CanonicalValue(std::move(bindings))},
      {"affected_controller_role_ids", CanonicalValue(std::move(controllers))},
      {"effect", CanonicalValue(std::string(route_effect(value.effect)))},
      {"error", value.error.has_value() ? error_value(*value.error) : CanonicalValue(nullptr)},
      {"latest_updated_at_ns", content_identity::nonnegative_int64_value(value.latest_updated_at_ns)},
      {"phase", CanonicalValue(std::string(route_phase(value.phase)))},
      {"reconciliation_required", CanonicalValue(value.reconciliation_required)},
      {"remaining_duration_ns", safe_signed(value.remaining_duration_ns)},
      {"resulting_active_control_configuration",
       value.resulting_active_control_configuration.has_value()
           ? active_configuration_value(*value.resulting_active_control_configuration)
           : CanonicalValue(nullptr)},
      {"status_owner_epoch", CanonicalValue(value.status_owner_epoch.value())},
      {"status_sequence", safe_unsigned(value.status_sequence)},
      {"terminal_result", value.terminal_result.has_value()
                              ? CanonicalValue(std::string(route_terminal(*value.terminal_result)))
                              : CanonicalValue(nullptr)}});
}

[[nodiscard]] CanonicalValue route_result_value(const CCLCRouteSelectionRequestResultDTO& value) {
  CanonicalValue::Object fields{
      {"acceptance", value.acceptance.has_value() ? route_acceptance_value(*value.acceptance)
                                                  : CanonicalValue(nullptr)},
      {"replay_status", CanonicalValue(std::string(to_string(value.replay_status)))},
      {"status",
       value.status.has_value() ? route_status_value(*value.status) : CanonicalValue(nullptr)}};
  if (value.rejection) fields.emplace_back("rejection", error_value(*value.rejection));
  return CanonicalValue(std::move(fields));
}

// Requested target and progress beside the snapshot (ADR-0026 §7,
// 2026-10-08). Neither is a snapshot member, digest input or admission input.
[[nodiscard]] CanonicalValue lifecycle_target_value(
    const std::optional<ComponentLifecycleTargetDTO>& value) {
  if (!value) return CanonicalValue(nullptr);
  return CanonicalValue(CanonicalValue::Object{
      {"origin", CanonicalValue(std::string(to_string(value->origin)))},
      {"requested_state", CanonicalValue(std::string(to_string(value->requested_state)))},
      {"target_generation", safe_unsigned(value->target_generation)}});
}

// Contract item 2 (addendum 2): the open incident, null when none. Times are
// full-width nonnegative owner-clock nanoseconds (spec 210 §1.1): a JSON
// number up to 2^53-1, a canonical decimal string above.
[[nodiscard]] CanonicalValue lifecycle_incident_value(
    const std::optional<ComponentLifecycleIncidentDTO>& value) {
  if (!value) return CanonicalValue(nullptr);
  if (!value->locally_valid() || value->acknowledged ||
      !content_identity::is_utf8_nfc(value->summary)) {
    throw std::invalid_argument("lifecycle incident is outside its closed shape");
  }
  CanonicalValue::Array codes;
  for (const auto& code : value->vendor_codes) codes.emplace_back(code);
  return CanonicalValue(CanonicalValue::Object{
      {"acknowledged", CanonicalValue(false)},
      {"cause_class", CanonicalValue(std::string(to_string(value->cause_class)))},
      {"detected_at_ns", content_identity::nonnegative_int64_value(value->detected_at_ns)},
      {"diagnostic", value->diagnostic ? ComponentIncidentDiagnosticValue(
                                             *value->diagnostic, value->diagnostic_snapshot
                                                                     ? *value->diagnostic_snapshot
                                                                     : CanonicalValue(nullptr))
                                       : CanonicalValue(nullptr)},
      {"incident_id", CanonicalValue(value->incident_id)},
      {"summary", CanonicalValue(value->summary)},
      {"vendor_codes", CanonicalValue(std::move(codes))}});
}

[[nodiscard]] CanonicalValue lifecycle_progress_value(
    const std::optional<ComponentLifecycleProgressDTO>& value) {
  if (!value) return CanonicalValue(nullptr);
  if ((value->blocking_reason && (value->blocking_reason->empty() ||
                                  value->blocking_reason->size() > 1024U)) ||
      (value->waiting_since_ns && *value->waiting_since_ns < 0)) {
    throw std::invalid_argument("lifecycle progress is outside its closed shape");
  }
  return CanonicalValue(CanonicalValue::Object{
      {"blocking_cause_class", CanonicalValue(std::string(to_string(value->blocking_cause_class)))},
      {"blocking_reason", value->blocking_reason ? CanonicalValue(*value->blocking_reason)
                                                 : CanonicalValue(nullptr)},
      {"delayed", CanonicalValue(value->delayed)},
      {"incident", lifecycle_incident_value(value->incident)},
      {"next_intent", value->next_intent
                          ? CanonicalValue(std::string(to_string(*value->next_intent)))
                          : CanonicalValue(nullptr)},
      {"phase", CanonicalValue(std::string(to_string(value->phase)))},
      {"waiting_since_ns",
       value->waiting_since_ns
           ? content_identity::nonnegative_int64_value(*value->waiting_since_ns)
           : CanonicalValue(nullptr)}});
}

[[nodiscard]] CanonicalValue component_snapshot_value(
    const ComponentLifecycleCurrentSnapshotDTO& value,
    const ComponentLifecycleTargetViewDTO& target_view) {
  CanonicalValue::Array domains;
  domains.reserve(value.domains.size());
  for (const auto& domain : value.domains) {
    CanonicalValue::Array bindings;
    bindings.reserve(domain.current_control_bindings.size());
    for (const auto& binding : domain.current_control_bindings) {
      bindings.emplace_back(CanonicalValue::Object{
          {"control_binding_id", CanonicalValue(binding.control_binding_id.value())},
          {"control_binding_revision", CanonicalValue(binding.control_binding_revision.value())},
          {"control_route_id", CanonicalValue(binding.control_route_id.value())}});
    }
    CanonicalValue::Array resources;
    resources.reserve(domain.current_resources.size());
    for (const auto& resource : domain.current_resources) {
      resources.emplace_back(resource.value());
    }
    domains.emplace_back(CanonicalValue::Object{
        {"active_control_configuration",
         domain.active_control_configuration.has_value()
             ? active_configuration_value(*domain.active_control_configuration)
             : CanonicalValue(nullptr)},
        {"control_domain_id", CanonicalValue(domain.control_domain_id.value())},
        {"control_domain_revision", CanonicalValue(domain.control_domain_revision.value())},
        {"controller_topology_id", CanonicalValue(domain.controller_topology_id.value())},
        {"current_control_bindings", CanonicalValue(std::move(bindings))},
        {"current_resources", CanonicalValue(std::move(resources))},
        {"topology_revision", CanonicalValue(domain.topology_revision.value())}});
  }
  CanonicalValue::Array providers;
  providers.reserve(value.provider_occurrences.size());
  for (const auto& provider : value.provider_occurrences) {
    providers.emplace_back(CanonicalValue::Object{
        {"control_domain_id", CanonicalValue(provider.control_domain_id.value())},
        {"lifecycle_plan_catalog_digest",
         CanonicalValue(provider.lifecycle_plan_catalog_digest.value())},
        {"provider_instance_id", CanonicalValue(provider.provider_instance_id.value())},
        {"provider_manifest_id", CanonicalValue(provider.provider_manifest_id.value())},
        {"provider_manifest_revision", CanonicalValue(provider.provider_manifest_revision.value())},
        {"provider_runtime_epoch", CanonicalValue(provider.provider_runtime_epoch.value())},
        {"provider_runtime_unit_id", CanonicalValue(provider.provider_runtime_unit_id.value())}});
  }
  const auto& publication = value.active_publication_receipt;
  CanonicalValue::Object fields{
      {"active_public_pointer_digest",
       CanonicalValue(publication.durable_public_pointer_content_digest.value())},
      {"component_lifecycle_generation",
       safe_unsigned(value.component_lifecycle_generation.value())},
      {"component_lifecycle_state",
       CanonicalValue(std::string(to_string(value.component_lifecycle_state)))},
      {"controllable_component_id", CanonicalValue(value.controllable_component_id.value())},
      {"domains", CanonicalValue(std::move(domains))},
      {"graph_revision", CanonicalValue(publication.request.candidate_graph_revision.value())},
      {"lifecycle_progress", lifecycle_progress_value(target_view.progress)},
      {"lifecycle_target", lifecycle_target_value(target_view.target)},
      {"provider_occurrences", CanonicalValue(std::move(providers))},
      {"publication_generation", safe_unsigned(publication.publication_generation.value())},
      {"retained_bundle_id",
       CanonicalValue(publication.request.candidate_retained_bundle_id.value())},
      {"snapshot_digest", CanonicalValue(value.snapshot_digest.value())}};
  if (target_view.dropped_output_lines.has_value() != target_view.dropped_output_bytes.has_value())
    throw std::invalid_argument("component output drop counters are incomplete");
  if (target_view.dropped_output_lines) {
    const auto count_value = [](std::uint64_t count) {
      return count <= kMaximumSafeUnsignedInteger ? safe_unsigned(count)
                                                  : CanonicalValue(std::to_string(count));
    };
    fields.emplace_back("dropped_output_lines", count_value(*target_view.dropped_output_lines));
    fields.emplace_back("dropped_output_bytes", count_value(*target_view.dropped_output_bytes));
  }
  return CanonicalValue(std::move(fields));
}

[[nodiscard]] CanonicalValue response_value(const std::string_view command, CanonicalValue result) {
  return CanonicalValue(CanonicalValue::Object{
      {"api_version", CanonicalValue(std::string(kCCLCOpenResponseApiVersion))},
      {"command", CanonicalValue(std::string(command))},
      {"error", CanonicalValue(nullptr)},
      {"ok", CanonicalValue(true)},
      {"result", std::move(result)}});
}

[[nodiscard]] CanonicalValue error_response_value(const std::string_view command,
                                                  const ErrorDTO& error) {
  return CanonicalValue(CanonicalValue::Object{
      {"api_version", CanonicalValue(std::string(kCCLCOpenResponseApiVersion))},
      {"command", CanonicalValue(std::string(command))},
      {"error", error_value(error)},
      {"ok", CanonicalValue(false)},
      {"result", CanonicalValue(nullptr)}});
}

[[nodiscard]] Result<std::string> encode_response(CanonicalValue value) {
  try {
    auto bytes = content_identity::canonical_json(value);
    if (bytes.size() > kMaximumCCLCOpenResponseBytes) {
      return Result<std::string>::Err(
          {"cclc_open_response_oversized", "CCLC open response exceeds its byte bound", {}});
    }
    return Result<std::string>::Ok(std::move(bytes));
  } catch (const std::exception& exception) {
    return Result<std::string>::Err({"cclc_open_response_invalid", exception.what(), {}});
  }
}

}  // namespace

bool CCLCRouteSelectionTemplateDTO::locally_valid() const noexcept {
  return RouteSelectionIntentDTO{
      {RequestId("cclc.template.validation"), RequestOwnerEpoch("cclc.template.validation"),
       "cclc-template-validation", std::nullopt},
      RetainedBundleId("cclc.template.validation"),
      GraphRevision("cclc.template.validation"),
      desired_configuration,
      expected_active_control_configuration,
      component_expectations,
      {}}
      .locally_valid();
}

bool CCLCRouteSelectionTemplateSetDTO::locally_valid() const noexcept {
  if (controllable_component_id.empty() || retained_bundle_id.empty() || graph_revision.empty() ||
      controllable_component_id.value().size() > kMaxRouteSelectionIdentityBytes ||
      retained_bundle_id.value().size() > kMaxRouteSelectionIdentityBytes ||
      graph_revision.value().size() > kMaxRouteSelectionIdentityBytes ||
      templates.size() > kMaxRouteSelectionComponents ||
      !std::is_sorted(templates.begin(), templates.end(), [](const auto& lhs, const auto& rhs) {
        return lhs.desired_configuration.control_domain_id <
               rhs.desired_configuration.control_domain_id;
      })) {
    return false;
  }
  std::optional<ControlDomainId> previous;
  for (const auto& row : templates) {
    if (!row.locally_valid() ||
        (previous.has_value() && *previous == row.desired_configuration.control_domain_id) ||
        std::none_of(row.component_expectations.begin(), row.component_expectations.end(),
                     [&](const auto& item) {
                       return item.controllable_component_id == controllable_component_id;
                     })) {
      return false;
    }
    previous = row.desired_configuration.control_domain_id;
  }
  return true;
}

ComponentLifecycleCoordinatorOpenRequestPort::ComponentLifecycleCoordinatorOpenRequestPort(
    ComponentLifecycleCoordinator& coordinator,
    const ICCLCCurrentComponentSnapshotSource& current_component_source,
    const ICCLCRouteSelectionTemplateSource& route_template_source,
    CCLCLifecycleTargetDriver* target_driver) noexcept
    : coordinator_(coordinator),
      current_component_source_(&current_component_source),
      route_template_source_(&route_template_source),
      target_driver_(target_driver) {}

ComponentLifecycleCoordinatorOpenRequestPort::ComponentLifecycleCoordinatorOpenRequestPort(
    ComponentLifecycleCoordinator& coordinator, CCLCLifecycleTargetDriver* target_driver) noexcept
    : coordinator_(coordinator), target_driver_(target_driver) {}

Result<ComponentLifecycleTargetViewDTO>
ComponentLifecycleCoordinatorOpenRequestPort::ReadLifecycleTargetView(
    const ControllableComponentId& component_id) const {
  if (target_driver_ == nullptr) {
    return Result<ComponentLifecycleTargetViewDTO>::Ok({});
  }
  return target_driver_->ReadTargetView(component_id);
}

RequestOwnerEpoch
ComponentLifecycleCoordinatorOpenRequestPort::ReadRequestOwnerEpoch() const {
  return coordinator_.request_owner_epoch();
}

Result<std::vector<ComponentIncidentEventDTO>>
ComponentLifecycleCoordinatorOpenRequestPort::ReadIncidentEvents(const std::uint64_t after_sequence,
                                                                 const std::size_t limit) {
  if (target_driver_ == nullptr) return Result<std::vector<ComponentIncidentEventDTO>>::Ok({});
  return target_driver_->LoadIncidentEvents(after_sequence, limit);
}

Result<CCLCLifecycleRequestResultDTO> ComponentLifecycleCoordinatorOpenRequestPort::SubmitLifecycle(
    const ComponentLifecycleIntentDTO& intent) {
  // The operator's Recover of an open incident is authorization-only: it is
  // recorded without a CCLC transaction, so a latched cell does not refuse it.
  // It joins the ordinary request membership, so its exact replay, a
  // conflicting reuse of its request id and a stale owner epoch are decided
  // below by ordinary CCLC admission like any other request. Every other
  // request (and a Recover without an open incident) is admitted unchanged.
  if (target_driver_ != nullptr) {
    auto recovered = target_driver_->RecoverIncident(intent);
    if (!recovered.ok()) {
      // During exchange, exact Recover replay remains a journal observation;
      // a genuinely new Recover is refused before its mutation or admission.
      if (recovered.error().code == "cclc_product_exchange_pending")
        return coordinator_.SubmitLifecycle(intent);
      return Result<CCLCLifecycleRequestResultDTO>::Err(recovered.error());
    }
    if (recovered.value()) {
      return Result<CCLCLifecycleRequestResultDTO>::Ok(std::move(*recovered.value()));
    }
  }
  auto result = coordinator_.SubmitLifecycle(intent);
  // The journal records an operator Prepare/Enable/Disable target in the
  // same commit as the acceptance; the driver only needs to look again.
  if (target_driver_ != nullptr && result.ok() &&
      result.value().replay_status == RequestReplayStatusDTO::kAcceptedNew &&
      result.value().acceptance.has_value()) {
    target_driver_->Wake();
  }
  return result;
}

Result<std::optional<CCLCLifecycleRequestResultDTO>>
ComponentLifecycleCoordinatorOpenRequestPort::ReadLifecycleRequestResult(
    const ComponentLifecycleIntentDTO& intent) {
  // An incident Recover's result is retained in the ordinary request ledger.
  return coordinator_.ReadLifecycleRequestResult(intent);
}

Result<std::optional<CCLCRouteSelectionRequestResultDTO>>
ComponentLifecycleCoordinatorOpenRequestPort::ReadRouteSelectionRequestResult(
    const RouteSelectionIntentDTO& intent) {
  return coordinator_.ReadRouteSelectionRequestResult(intent);
}

Result<std::optional<ComponentLifecycleStatusDTO>>
ComponentLifecycleCoordinatorOpenRequestPort::LoadLifecycleStatus(
    const LifecycleTransactionId& transaction_id) {
  return coordinator_.LoadLifecycleStatus(transaction_id);
}

Result<ComponentLifecycleCurrentSnapshotDTO>
ComponentLifecycleCoordinatorOpenRequestPort::ReadCurrentComponent(
    const ControllableComponentId& component_id) const {
  if (current_component_source_)
    return current_component_source_->ReadCurrentComponentSnapshot(component_id);
  auto pin = coordinator_.AcquireProductPin();
  if (!pin.ok()) return Result<ComponentLifecycleCurrentSnapshotDTO>::Err(pin.error());
  return pin.value().context().current_snapshot_source.ReadCurrentComponentSnapshot(component_id);
}

Result<CCLCRouteSelectionTemplateSetDTO>
ComponentLifecycleCoordinatorOpenRequestPort::ReadRouteSelectionTemplates(
    const ControllableComponentId& component_id) const {
  if (route_template_source_)
    return route_template_source_->ReadCurrentRouteSelectionTemplates(component_id);
  auto pin = coordinator_.AcquireProductPin();
  if (!pin.ok()) return Result<CCLCRouteSelectionTemplateSetDTO>::Err(pin.error());
  const auto* source = pin.value().context().route_template_source;
  if (source == nullptr)
    return Result<CCLCRouteSelectionTemplateSetDTO>::Err(
        {"cclc_route_templates_unavailable", "route templates are not composed", {}});
  return source->ReadCurrentRouteSelectionTemplates(component_id);
}

Result<CCLCRouteSelectionRequestResultDTO>
ComponentLifecycleCoordinatorOpenRequestPort::SubmitRouteSelection(
    const RouteSelectionIntentDTO& intent) {
  return coordinator_.SubmitRouteSelection(intent);
}

Result<std::optional<RouteSelectionStatusDTO>>
ComponentLifecycleCoordinatorOpenRequestPort::LoadRouteSelectionStatus(
    const RouteSelectionTransactionId& transaction_id) {
  return coordinator_.LoadRouteSelectionStatus(transaction_id);
}

Result<std::string> ExecuteCanonicalCCLCOpenRequest(const std::string_view canonical_request,
                                                    ICCLCOpenRequestPort& port,
                                                    RequestAttributionDTO transport_attribution) {
  std::string command("invalid");
  try {
    if (canonical_request.empty() || canonical_request.size() > kMaximumCCLCOpenRequestBytes ||
        !transport_attribution.locally_valid()) {
      fail("CCLC open request envelope is invalid");
    }
    const auto root = content_identity::parse_canonical_json(
        canonical_request,
        {kMaximumCCLCOpenRequestBytes, content_identity::kMaximumCanonicalJsonDepth,
         content_identity::kMaximumCanonicalJsonNodes});
    exact_object(root, {"api_version", "command", "payload"}, {}, "CCLC open request");
    if (text(member(root, "api_version", "CCLC open request"), "api_version") !=
        kCCLCOpenRequestApiVersion) {
      fail("CCLC open request API version is unsupported");
    }
    command = text(member(root, "command", "CCLC open request"), "command");
    const auto& payload = member(root, "payload", "CCLC open request");

    if (command == "owner_identity") {
      exact_object(payload, {}, {}, "owner identity request");
      const auto owner_epoch = port.ReadRequestOwnerEpoch();
      if (owner_epoch.empty()) {
        fail("CCLC owner identity is invalid");
      }
      return encode_response(response_value(
          command,
          CanonicalValue(CanonicalValue::Object{
              {"request_owner_epoch", CanonicalValue(owner_epoch.value())}})));
    }

    if (command == "component_snapshot") {
      exact_object(payload, {"controllable_component_id"}, {}, "component snapshot request");
      auto result = port.ReadCurrentComponent(ControllableComponentId(
          text(member(payload, "controllable_component_id", "component snapshot request"),
               "controllable_component_id")));
      if (!result.ok()) {
        return encode_response(error_response_value(command, result.error()));
      }
      const auto verified = VerifyComponentLifecycleCurrentSnapshot(result.value());
      if (!verified.ok()) {
        return encode_response(error_response_value(command, verified.error()));
      }
      auto target_view = port.ReadLifecycleTargetView(result.value().controllable_component_id);
      if (!target_view.ok()) {
        return encode_response(error_response_value(command, target_view.error()));
      }
      return encode_response(response_value(
          command, component_snapshot_value(result.value(), target_view.value())));
    }
    if (command == "incident_events") {
      exact_object(payload, {"after_sequence", "limit"}, {}, "incident events request");
      const auto after = content_identity::parse_nonnegative_int64(
          member(payload, "after_sequence", "incident events request"));
      const auto limit = content_identity::parse_nonnegative_int64(
          member(payload, "limit", "incident events request"));
      if (limit < 1 || limit > static_cast<std::int64_t>(kMaxComponentIncidentEventPage)) {
        fail("incident events limit is outside its bound");
      }
      auto result = port.ReadIncidentEvents(static_cast<std::uint64_t>(after),
                                            static_cast<std::size_t>(limit));
      if (!result.ok()) {
        return encode_response(error_response_value(command, result.error()));
      }
      CanonicalValue::Array events;
      auto through = static_cast<std::uint64_t>(after);
      const auto envelope_bytes =
          content_identity::canonical_json(
              response_value(
                  command, CanonicalValue(CanonicalValue::Object{
                               {"events", CanonicalValue(CanonicalValue::Array{})},
                               {"through_sequence", safe_unsigned(kMaximumSafeUnsignedInteger)}})))
              .size();
      auto remaining_bytes = kMaximumCCLCOpenResponseBytes - envelope_bytes;
      for (const auto& event : result.value()) {
        // Owner-side shape: a negative time is an owner failure, not a
        // caller error, and is never clamped.
        if (event.occurred_at_ns < 0) {
          throw std::invalid_argument("incident event is outside its closed shape");
        }
        auto value = CanonicalValue(CanonicalValue::Object{
            {"event_kind", CanonicalValue(event.event_kind)},
            {"event_sequence", safe_unsigned(event.sequence)},
            {"incident_id", CanonicalValue(event.incident_id)},
            {"occurred_at_ns", content_identity::nonnegative_int64_value(event.occurred_at_ns)},
            {"payload", content_identity::parse_canonical_json(event.payload_json)}});
        const auto value_bytes =
            content_identity::canonical_json(value).size() + (events.empty() ? 0U : 1U);
        if (value_bytes > remaining_bytes) {
          if (events.empty())
            throw std::invalid_argument("one incident event exceeds the response bound");
          break;  // At-most-limit page: complete events, with the last emitted cursor.
        }
        remaining_bytes -= value_bytes;
        events.push_back(std::move(value));
        through = std::max(through, event.sequence);
      }
      return encode_response(response_value(
          command, CanonicalValue(CanonicalValue::Object{
                       {"events", CanonicalValue(std::move(events))},
                       {"through_sequence", safe_unsigned(through)}})));
    }
    if (command == "route_templates") {
      exact_object(payload, {"controllable_component_id"}, {}, "route template request");
      auto result = port.ReadRouteSelectionTemplates(ControllableComponentId(
          text(member(payload, "controllable_component_id", "route template request"),
               "controllable_component_id")));
      if (!result.ok()) {
        return encode_response(error_response_value(command, result.error()));
      }
      if (!result.value().locally_valid()) {
        fail("CCLC route template result is invalid");
      }
      return encode_response(response_value(command, route_template_set_value(result.value())));
    }
    if (command == "lifecycle_submit") {
      auto result = port.SubmitLifecycle(lifecycle_intent(payload, transport_attribution));
      if (!result.ok()) {
        return encode_response(error_response_value(command, result.error()));
      }
      if (!result.value().locally_valid()) {
        fail("CCLC lifecycle result is invalid");
      }
      return encode_response(response_value(command, lifecycle_result_value(result.value())));
    }
    if (command == "lifecycle_request_result") {
      auto result =
          port.ReadLifecycleRequestResult(lifecycle_intent(payload, transport_attribution));
      if (!result.ok()) return encode_response(error_response_value(command, result.error()));
      if (result.value() && !result.value()->locally_valid())
        fail("CCLC lifecycle result is invalid");
      return encode_response(response_value(command, result.value()
                                                         ? lifecycle_result_value(*result.value())
                                                         : CanonicalValue(nullptr)));
    }
    if (command == "lifecycle_status") {
      exact_object(payload, {"lifecycle_transaction_id"}, {}, "lifecycle status request");
      auto result = port.LoadLifecycleStatus(LifecycleTransactionId(
          text(member(payload, "lifecycle_transaction_id", "lifecycle status request"),
               "lifecycle_transaction_id")));
      if (!result.ok()) {
        return encode_response(error_response_value(command, result.error()));
      }
      CanonicalValue encoded = result.value().has_value() ? lifecycle_status_value(*result.value())
                                                          : CanonicalValue(nullptr);
      return encode_response(response_value(command, std::move(encoded)));
    }
    if (command == "route_submit") {
      auto result = port.SubmitRouteSelection(route_intent(payload, transport_attribution));
      if (!result.ok()) {
        return encode_response(error_response_value(command, result.error()));
      }
      if (!result.value().locally_valid()) {
        fail("CCLC route result is invalid");
      }
      return encode_response(response_value(command, route_result_value(result.value())));
    }
    if (command == "route_request_result") {
      auto result =
          port.ReadRouteSelectionRequestResult(route_intent(payload, transport_attribution));
      if (!result.ok()) return encode_response(error_response_value(command, result.error()));
      if (result.value() && !result.value()->locally_valid()) fail("CCLC route result is invalid");
      return encode_response(response_value(
          command, result.value() ? route_result_value(*result.value()) : CanonicalValue(nullptr)));
    }
    if (command == "route_status") {
      exact_object(payload, {"route_selection_transaction_id"}, {}, "route status request");
      auto result = port.LoadRouteSelectionStatus(RouteSelectionTransactionId(
          text(member(payload, "route_selection_transaction_id", "route status request"),
               "route_selection_transaction_id")));
      if (!result.ok()) {
        return encode_response(error_response_value(command, result.error()));
      }
      CanonicalValue encoded = result.value().has_value() ? route_status_value(*result.value())
                                                          : CanonicalValue(nullptr);
      return encode_response(response_value(command, std::move(encoded)));
    }
    fail("CCLC open request command is unsupported");
  } catch (const DecodeFailure& exception) {
    return encode_response(error_response_value(command, {"cclc_open_request_invalid",
                                                          "CCLC open request is invalid",
                                                          {{"reason", exception.what()}}}));
  } catch (const content_identity::CanonicalValueError& exception) {
    return encode_response(error_response_value(command, {"cclc_open_request_invalid",
                                                          "CCLC open request is invalid",
                                                          {{"reason", exception.what()}}}));
  } catch (const std::exception& exception) {
    return Result<std::string>::Err({"cclc_open_request_failure", exception.what(), {}});
  } catch (...) {
    return Result<std::string>::Err({"cclc_open_request_failure", "CCLC open request failed", {}});
  }
}

}  // namespace manyforge::core
