#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <manyforge_content_identity/canonical_value.hpp>
#include <manyforge_content_identity/content_identity.hpp>
#include <manyforge_core/cclc_open_request.hpp>
#include <optional>
#include <string>
#include <utility>

namespace manyforge::core {
namespace {

using content_identity::CanonicalValue;

/// 2^53 + 1: the first owner-clock value a JSON number cannot carry exactly.
constexpr std::int64_t kBeyondSafeNs = 9'007'199'254'740'993LL;
constexpr std::int64_t kMaximumClockNs = std::numeric_limits<std::int64_t>::max();

[[nodiscard]] CanonicalValue ContextValue(const std::string& request_id) {
  return CanonicalValue(
      CanonicalValue::Object{{"request_id", CanonicalValue(request_id)},
                             {"request_owner_epoch", CanonicalValue("client.epoch.1")},
                             {"source_label", CanonicalValue("test-client")}});
}

[[nodiscard]] std::string Request(std::string command, CanonicalValue payload) {
  return content_identity::canonical_json(CanonicalValue(CanonicalValue::Object{
      {"api_version", CanonicalValue(std::string(kCCLCOpenRequestApiVersion))},
      {"command", CanonicalValue(std::move(command))},
      {"payload", std::move(payload)}}));
}

[[nodiscard]] CanonicalValue LifecycleIntentValue() {
  return CanonicalValue(CanonicalValue::Object{
      {"controllable_component_id", CanonicalValue("component.arm")},
      {"expected_component_lifecycle_generation", CanonicalValue(4.0)},
      {"expected_admission_generation", CanonicalValue(7.0)},
      {"expected_control_domains",
       CanonicalValue(CanonicalValue::Array{CanonicalValue(CanonicalValue::Object{
           {"kind", CanonicalValue("controller")},
           {"control_domain_id", CanonicalValue("domain.arm")},
           {"control_domain_revision", CanonicalValue("domain.revision.1")},
           {"controller_topology_id", CanonicalValue("topology.arm")},
           {"expected_active_control_configuration",
            CanonicalValue(CanonicalValue::Object{
                {"active_control_configuration_generation", CanonicalValue(7.0)},
                {"active_control_configuration_id", CanonicalValue("active.arm.7")},
                {"presence", CanonicalValue("present")}})},
           {"expected_control_bindings",
            CanonicalValue(CanonicalValue::Array{CanonicalValue(CanonicalValue::Object{
                {"control_binding_id", CanonicalValue("binding.arm")},
                {"control_binding_revision", CanonicalValue("binding.revision.1")},
                {"control_route_id", CanonicalValue("route.arm")}})})},
           {"topology_revision", CanonicalValue("topology.revision.1")}})})},
      {"expected_graph_revision", CanonicalValue("graph.revision.1")},
      {"intent", CanonicalValue("enable")},
      {"reason", CanonicalValue("operator-confirmed")},
      {"request_context", ContextValue("request.lifecycle.1")}});
}

[[nodiscard]] CanonicalValue RouteIntentValue(bool admission = true) {
  CanonicalValue::Object fields{
      {"component_expectations",
       CanonicalValue(CanonicalValue::Array{CanonicalValue(CanonicalValue::Object{
           {"controllable_component_id", CanonicalValue("component.arm")},
           {"expected_component_lifecycle_generation", CanonicalValue(4.0)},
           {"expected_component_lifecycle_state", CanonicalValue("prepared")}})})},
      {"desired_configuration",
       CanonicalValue(CanonicalValue::Object{
           {"activation_set_id", CanonicalValue("activation.arm")},
           {"binding_routes",
            CanonicalValue(CanonicalValue::Array{CanonicalValue(CanonicalValue::Object{
                {"control_binding_id", CanonicalValue("binding.arm")},
                {"control_binding_revision", CanonicalValue("binding.revision.1")},
                {"control_route_id", CanonicalValue("route.arm")}})})},
           {"control_domain_id", CanonicalValue("domain.arm")},
           {"control_domain_revision", CanonicalValue("domain.revision.1")},
           {"controller_topology_id", CanonicalValue("topology.arm")},
           {"topology_revision", CanonicalValue("topology.revision.1")}})},
      {"expected_active_control_configuration",
       CanonicalValue(CanonicalValue::Object{{"presence", CanonicalValue("absent")}})},
      {"expected_graph_revision", CanonicalValue("graph.revision.1")},
      {"expected_retained_bundle_id", CanonicalValue("bundle.1")},
      {"request_context", ContextValue("request.route.1")}};
  if (admission) fields.emplace_back("expected_admission_generation", CanonicalValue(9.0));
  return CanonicalValue(std::move(fields));
}

[[nodiscard]] const CanonicalValue& Member(const CanonicalValue& value,
                                           const std::string_view name) {
  const auto* member = value.find_object_member(name);
  if (member == nullptr) {
    throw std::logic_error("missing test response member");
  }
  return *member;
}

class FakeOpenPort final : public ICCLCOpenRequestPort {
 public:
  RequestOwnerEpoch ReadRequestOwnerEpoch() const override {
    return RequestOwnerEpoch("cclc-owner.test");
  }

  Result<CCLCLifecycleRequestResultDTO> SubmitLifecycle(
      const ComponentLifecycleIntentDTO& intent) override {
    lifecycle_intent = intent;
    return Result<CCLCLifecycleRequestResultDTO>::Ok(
        {RequestReplayStatusDTO::kRequestIdConflict, std::nullopt, std::nullopt});
  }

  Result<CCLCLifecycleRequestResultDTO> SubmitLifecycleWithoutIncidentAcknowledgement(
      const ComponentLifecycleIntentDTO& intent) override {
    no_acknowledgement_intent = intent;
    if (incident_open) {
      return Result<CCLCLifecycleRequestResultDTO>::Err(
          {"cclc_incident_open", "a component incident is open",
           {{"effect", "not_started"}, {"reason", "incident_open"}}});
    }
    return SubmitLifecycle(intent);
  }
  std::optional<ComponentLifecycleIntentDTO> no_acknowledgement_intent;
  bool incident_open{};

  Result<CCLCLifecycleRequestResultDTO> SubmitLifecycleExpectingIncident(
      const ComponentLifecycleIntentDTO& intent, const std::string& expected) override {
    expected_incident_id = expected;
    if (expected != open_incident_id) {
      return Result<CCLCLifecycleRequestResultDTO>::Err(
          {"cclc_incident_changed", "the component's open incident is another one",
           {{"effect", "not_started"},
            {"reason", "incident_changed"},
            {"expected_incident_id", expected},
            {"incident_id", open_incident_id}}});
    }
    return SubmitLifecycle(intent);
  }
  std::optional<std::string> expected_incident_id;
  std::string open_incident_id{"incident.b"};

  Result<std::optional<ComponentLifecycleStatusDTO>> LoadLifecycleStatus(
      const LifecycleTransactionId& transaction_id) override {
    lifecycle_status_id = transaction_id;
    return Result<std::optional<ComponentLifecycleStatusDTO>>::Ok(lifecycle_status);
  }

  Result<ComponentLifecycleCurrentSnapshotDTO> ReadCurrentComponent(
      const ControllableComponentId& component_id) const override {
    component_id_read = component_id;
    return Result<ComponentLifecycleCurrentSnapshotDTO>::Err(
        {"missing_reference", "component snapshot is absent", {}});
  }

  Result<CCLCComponentReadDTO> ReadCurrentComponentView(
      const ControllableComponentId& component_id) const override {
    const auto result = ReadCurrentComponent(component_id);
    return Result<CCLCComponentReadDTO>::Err(result.error());
  }

  Result<CCLCRouteSelectionTemplateSetDTO> ReadRouteSelectionTemplates(
      const ControllableComponentId& component_id) const override {
    route_template_component_id = component_id;
    return Result<CCLCRouteSelectionTemplateSetDTO>::Ok(
        {component_id, RetainedBundleId("bundle.1"), GraphRevision("graph.1"), {}});
  }

  Result<CCLCRouteSelectionRequestResultDTO> SubmitRouteSelection(
      const RouteSelectionIntentDTO& intent) override {
    route_intent = intent;
    return Result<CCLCRouteSelectionRequestResultDTO>::Ok(
        {RequestReplayStatusDTO::kRequestLedgerFull, std::nullopt, std::nullopt});
  }

  Result<std::optional<RouteSelectionStatusDTO>> LoadRouteSelectionStatus(
      const RouteSelectionTransactionId& transaction_id) override {
    route_status_id = transaction_id;
    return Result<std::optional<RouteSelectionStatusDTO>>::Ok(std::nullopt);
  }

  Result<std::optional<CCLCRouteSelectionRequestResultDTO>> ReadRouteSelectionRequestResult(
      const RouteSelectionIntentDTO& intent) override {
    route_result_intent = intent;
    return Result<std::optional<CCLCRouteSelectionRequestResultDTO>>::Ok(std::nullopt);
  }

  Result<CCLCLifecycleTargetRetirementOfferDTO> ReadLifecycleTargetRetirementOffer() override {
    return Result<CCLCLifecycleTargetRetirementOfferDTO>::Ok({retirement_scope, retirement_rows});
  }
  Result<CCLCLifecycleTargetRetirementResultDTO> RetireLifecycleTargetsForClose(
      const CCLCLifecycleTargetRetirementIntentDTO& intent) override {
    ++retirement_writes;
    retirement_request = intent;
    return Result<CCLCLifecycleTargetRetirementResultDTO>::Ok(
        {RequestReplayStatusDTO::kAcceptedNew, intent.scope, intent.expected_targets, intent.request_context});
  }
  Result<std::optional<CCLCLifecycleTargetRetirementResultDTO>> ReadLifecycleTargetRetirementResult(
      const CCLCLifecycleTargetRetirementIntentDTO& intent) override {
    retirement_read = intent;
    return Result<std::optional<CCLCLifecycleTargetRetirementResultDTO>>::Ok(std::nullopt);
  }
  Result<void> ReadLifecycleOwnerSettlement(const CCLCLifecycleTargetRetirementScopeDTO& scope) override {
    settlement_scope = scope;
    if (settlement_unknown)
      return Result<void>::Err({"cclc_owner_unsettled", "actual owner settlement is unavailable", {}});
    return Result<void>::Ok();
  }
  CCLCLifecycleTargetRetirementScopeDTO retirement_scope{
      RequestOwnerEpoch("client.epoch.1"), ActiveBundlePublicationGeneration(7U),
      ContentDigest("sha256:" + std::string(64U, 'a'))};
  std::vector<ComponentLifecycleTargetDTO> retirement_rows{
      {ControllableComponentId("component.arm"), LifecycleTargetStateDTO::kActive,
       LifecycleTargetOriginDTO::kOperator, 5U, false, LifecycleBlockingCauseClassDTO::kFault}};
  int retirement_writes{};
  bool settlement_unknown{true};
  std::optional<CCLCLifecycleTargetRetirementIntentDTO> retirement_request;
  std::optional<CCLCLifecycleTargetRetirementIntentDTO> retirement_read;
  std::optional<CCLCLifecycleTargetRetirementScopeDTO> settlement_scope;

  std::optional<ComponentLifecycleIntentDTO> lifecycle_intent;
  std::optional<RouteSelectionIntentDTO> route_intent;
  std::optional<RouteSelectionIntentDTO> route_result_intent;
  std::optional<LifecycleTransactionId> lifecycle_status_id;
  std::optional<ComponentLifecycleStatusDTO> lifecycle_status;
  std::optional<RouteSelectionTransactionId> route_status_id;
  mutable std::optional<ControllableComponentId> component_id_read;
  mutable std::optional<ControllableComponentId> route_template_component_id;
};

TEST(CCLCOpenRequest, ProjectsTheSoleRequestOwnerIdentity) {
  FakeOpenPort port;
  const auto result = ExecuteCanonicalCCLCOpenRequest(
      Request("owner_identity", CanonicalValue(CanonicalValue::Object{})), port,
      {RequestTransportDTO::kHttp, RequestAssuranceDTO::kUnverified,
       std::nullopt});
  ASSERT_TRUE(result.ok()) << result.error().message;
  const auto response = content_identity::parse_canonical_json(result.value());
  EXPECT_EQ(Member(Member(response, "result"), "request_owner_epoch")
                .as_string(),
            "cclc-owner.test");
}

TEST(CCLCOpenRequest, StampsTransportAttributionAndPreservesLifecycleReplayResult) {
  FakeOpenPort port;
  const auto result = ExecuteCanonicalCCLCOpenRequest(
      Request("lifecycle_submit", LifecycleIntentValue()), port,
      {RequestTransportDTO::kHttp, RequestAssuranceDTO::kUnverified, std::nullopt});
  ASSERT_TRUE(result.ok()) << result.error().message;
  ASSERT_TRUE(port.lifecycle_intent.has_value());
  EXPECT_EQ(port.lifecycle_intent->attribution.transport, RequestTransportDTO::kHttp);
  EXPECT_EQ(port.lifecycle_intent->attribution.assurance, RequestAssuranceDTO::kUnverified);
  EXPECT_EQ(port.lifecycle_intent->request_context.request_id, RequestId("request.lifecycle.1"));

  const auto response = content_identity::parse_canonical_json(result.value());
  EXPECT_TRUE(Member(response, "ok").as_bool());
  EXPECT_EQ(Member(response, "command").as_string(), "lifecycle_submit");
  EXPECT_EQ(Member(Member(response, "result"), "replay_status").as_string(), "request_id_conflict");
  EXPECT_TRUE(Member(Member(response, "result"), "acceptance").is_null());
}

// M6: `acknowledge_incident: false` never reaches the acknowledging Recover;
// absent or true keeps today's path.
TEST(CCLCOpenRequest, LifecycleWithoutIncidentAcknowledgementIsRoutedAndRefusedTyped) {
  auto fields = LifecycleIntentValue().as_object();
  for (auto& [name, value] : fields) {
    if (name == "intent") value = CanonicalValue("recover");
  }
  const auto with = [&](std::optional<CanonicalValue> acknowledge) {
    auto copy = fields;
    if (acknowledge) copy.emplace_back("acknowledge_incident", *acknowledge);
    return Request("lifecycle_submit", CanonicalValue(copy));
  };
  {
    FakeOpenPort port;
    ASSERT_TRUE(ExecuteCanonicalCCLCOpenRequest(with(std::nullopt), port, {}).ok());
    EXPECT_TRUE(port.lifecycle_intent);
    EXPECT_FALSE(port.no_acknowledgement_intent);
  }
  {
    FakeOpenPort port;
    ASSERT_TRUE(ExecuteCanonicalCCLCOpenRequest(with(CanonicalValue(true)), port, {}).ok());
    EXPECT_FALSE(port.no_acknowledgement_intent);
  }
  {
    // An incident opened before the request arrived: refused typed, no effect.
    FakeOpenPort port;
    port.incident_open = true;
    const auto refused = ExecuteCanonicalCCLCOpenRequest(with(CanonicalValue(false)), port, {});
    ASSERT_TRUE(refused.ok());
    const auto response = content_identity::parse_canonical_json(refused.value());
    EXPECT_FALSE(Member(response, "ok").as_bool());
    EXPECT_EQ(Member(Member(response, "error"), "code").as_string(), "cclc_incident_open");
    EXPECT_TRUE(port.no_acknowledgement_intent);
    EXPECT_FALSE(port.lifecycle_intent);
  }
  {
    FakeOpenPort port;
    const auto malformed = ExecuteCanonicalCCLCOpenRequest(with(CanonicalValue("no")), port, {});
    ASSERT_TRUE(malformed.ok());
    EXPECT_FALSE(Member(content_identity::parse_canonical_json(malformed.value()), "ok").as_bool());
    EXPECT_FALSE(port.lifecycle_intent);
    EXPECT_FALSE(port.no_acknowledgement_intent);
  }
}

// M6 (item 36): `expected_incident_id` routes the Recover through the
// expected-incident check; a replaced incident is refused typed, naming the
// current one; omitted keeps today's path.
TEST(CCLCOpenRequest, RecoverExpectingAnIncidentIsRoutedAndRefusedTypedWhenReplaced) {
  auto fields = LifecycleIntentValue().as_object();
  for (auto& [name, value] : fields) {
    if (name == "intent") value = CanonicalValue("recover");
  }
  const auto with = [&](std::optional<std::string> expected,
                        CanonicalValue::Object base) {
    if (expected) base.emplace_back("expected_incident_id", CanonicalValue(*expected));
    return Request("lifecycle_submit", CanonicalValue(base));
  };
  {
    // Omitted: today's Recover, no check.
    FakeOpenPort port;
    ASSERT_TRUE(ExecuteCanonicalCCLCOpenRequest(with(std::nullopt, fields), port, {}).ok());
    EXPECT_TRUE(port.lifecycle_intent);
    EXPECT_FALSE(port.expected_incident_id);
  }
  {
    // A was replaced by B: refused typed, the details name B; nothing submitted.
    FakeOpenPort port;
    const auto refused =
        ExecuteCanonicalCCLCOpenRequest(with(std::string("incident.a"), fields), port, {});
    ASSERT_TRUE(refused.ok());
    const auto response = content_identity::parse_canonical_json(refused.value());
    EXPECT_FALSE(Member(response, "ok").as_bool());
    const auto& error = Member(response, "error");
    EXPECT_EQ(Member(error, "code").as_string(), "cclc_incident_changed");
    EXPECT_EQ(Member(Member(error, "details"), "incident_id").as_string(), "incident.b");
    EXPECT_EQ(Member(Member(error, "details"), "effect").as_string(), "not_started");
    EXPECT_EQ(port.expected_incident_id, "incident.a");
    EXPECT_FALSE(port.lifecycle_intent);
  }
  {
    // Expecting the current incident: the ordinary Recover follows.
    FakeOpenPort port;
    const auto accepted =
        ExecuteCanonicalCCLCOpenRequest(with(std::string("incident.b"), fields), port, {});
    ASSERT_TRUE(accepted.ok());
    EXPECT_TRUE(Member(content_identity::parse_canonical_json(accepted.value()), "ok").as_bool());
    EXPECT_EQ(port.expected_incident_id, "incident.b");
    EXPECT_TRUE(port.lifecycle_intent);
  }
  {
    // Only an acknowledging ordinary Recover may carry it.
    auto enable = LifecycleIntentValue().as_object();
    auto no_ack = fields;
    no_ack.emplace_back("acknowledge_incident", CanonicalValue(false));
    for (const auto& base : {enable, no_ack}) {
      FakeOpenPort port;
      const auto malformed =
          ExecuteCanonicalCCLCOpenRequest(with(std::string("incident.b"), base), port, {});
      ASSERT_TRUE(malformed.ok());
      EXPECT_FALSE(Member(content_identity::parse_canonical_json(malformed.value()), "ok").as_bool());
      EXPECT_FALSE(port.lifecycle_intent);
      EXPECT_FALSE(port.no_acknowledgement_intent);
      EXPECT_FALSE(port.expected_incident_id);
    }
  }
}

TEST(CCLCOpenRequest, CloseRecoveryIsClosedTypedAndCannotAliasOrdinaryRecover) {
  FakeOpenPort port;
  auto fields = LifecycleIntentValue().as_object();
  for (auto& [name, value] : fields) {
    if (name == "intent") value = CanonicalValue("recover");
    if (name == "reason") value = CanonicalValue("cell closed by operator");
  }
  fields.emplace_back("close_incident_recovery", CanonicalValue(CanonicalValue::Object{
      {"expected_incident_id", CanonicalValue("incident.exact")}}));
  const auto encoded = Request("lifecycle_submit", CanonicalValue(fields));
  const auto submitted = ExecuteCanonicalCCLCOpenRequest(encoded, port, {});
  ASSERT_TRUE(submitted.ok());
  ASSERT_TRUE(port.lifecycle_intent);
  ASSERT_TRUE(port.lifecycle_intent->close_incident_recovery);
  EXPECT_EQ(port.lifecycle_intent->close_incident_recovery->expected_incident_id, "incident.exact");
  port.lifecycle_intent.reset();
  for (auto& [name, value] : fields) {
    if (name == "close_incident_recovery")
      value = CanonicalValue(CanonicalValue::Object{
          {"expected_incident_id", CanonicalValue("incident.exact")},
          {"ordinary_fallback", CanonicalValue(true)}});
  }
  const auto refused = ExecuteCanonicalCCLCOpenRequest(
      Request("lifecycle_submit", CanonicalValue(fields)), port, {});
  ASSERT_TRUE(refused.ok());
  EXPECT_FALSE(Member(content_identity::parse_canonical_json(refused.value()), "ok").as_bool());
  EXPECT_FALSE(port.lifecycle_intent);
}

TEST(CCLCOpenRequest, PreservesCompleteRouteIntentAndReturnsBoundedAdmissionFact) {
  FakeOpenPort port;
  const auto result = ExecuteCanonicalCCLCOpenRequest(
      Request("route_submit", RouteIntentValue()), port,
      {RequestTransportDTO::kRos, RequestAssuranceDTO::kUnverified, std::nullopt});
  ASSERT_TRUE(result.ok()) << result.error().message;
  ASSERT_TRUE(port.route_intent.has_value());
  EXPECT_EQ(port.route_intent->expected_admission_generation, AdmissionGeneration(9U));
  EXPECT_EQ(port.route_intent->attribution.transport, RequestTransportDTO::kRos);
  EXPECT_EQ(port.route_intent->desired_configuration.control_domain_id,
            ControlDomainId("domain.arm"));
  ASSERT_EQ(port.route_intent->desired_configuration.binding_routes.size(), 1U);

  const auto response = content_identity::parse_canonical_json(result.value());
  EXPECT_TRUE(Member(response, "ok").as_bool());
  EXPECT_EQ(Member(Member(response, "result"), "replay_status").as_string(), "request_ledger_full");
  EXPECT_EQ(Member(response, "result").as_object().size(), 3U);
  EXPECT_EQ(Member(response, "result").find_object_member("rejection"), nullptr);
}

TEST(CCLCOpenRequest, StatusAndSnapshotReadsCarryNoClientOwnedTruth) {
  FakeOpenPort port;
  const auto original = ExecuteCanonicalCCLCOpenRequest(
      Request("route_request_result", RouteIntentValue(false)), port, {});
  ASSERT_TRUE(original.ok());
  EXPECT_TRUE(Member(content_identity::parse_canonical_json(original.value()), "result").is_null());
  ASSERT_TRUE(port.route_result_intent);
  EXPECT_FALSE(port.route_result_intent->expected_admission_generation);
  EXPECT_EQ(port.route_result_intent->request_context.request_id, RequestId("request.route.1"));
  EXPECT_FALSE(port.route_intent);  // No submit/admission call during observation.
  const auto lifecycle = ExecuteCanonicalCCLCOpenRequest(
      Request("lifecycle_status",
              CanonicalValue(CanonicalValue::Object{
                  {"lifecycle_transaction_id", CanonicalValue("lifecycle.7")}})),
      port, {});
  ASSERT_TRUE(lifecycle.ok());
  ASSERT_EQ(port.lifecycle_status_id, LifecycleTransactionId("lifecycle.7"));
  EXPECT_TRUE(
      Member(content_identity::parse_canonical_json(lifecycle.value()), "result").is_null());

  const auto route = ExecuteCanonicalCCLCOpenRequest(
      Request("route_status", CanonicalValue(CanonicalValue::Object{
                                  {"route_selection_transaction_id", CanonicalValue("route.9")}})),
      port, {});
  ASSERT_TRUE(route.ok());
  ASSERT_EQ(port.route_status_id, RouteSelectionTransactionId("route.9"));

  const auto snapshot = ExecuteCanonicalCCLCOpenRequest(
      Request("component_snapshot",
              CanonicalValue(CanonicalValue::Object{
                  {"controllable_component_id", CanonicalValue("component.arm")}})),
      port, {});
  ASSERT_TRUE(snapshot.ok());
  ASSERT_EQ(port.component_id_read, ControllableComponentId("component.arm"));
  const auto response = content_identity::parse_canonical_json(snapshot.value());
  EXPECT_FALSE(Member(response, "ok").as_bool());
  EXPECT_EQ(Member(Member(response, "error"), "code").as_string(), "missing_reference");

  const auto templates = ExecuteCanonicalCCLCOpenRequest(
      Request("route_templates",
              CanonicalValue(CanonicalValue::Object{
                  {"controllable_component_id", CanonicalValue("component.arm")}})),
      port, {});
  ASSERT_TRUE(templates.ok());
  ASSERT_EQ(port.route_template_component_id, ControllableComponentId("component.arm"));
  const auto template_response = content_identity::parse_canonical_json(templates.value());
  EXPECT_TRUE(Member(template_response, "ok").as_bool());
  EXPECT_TRUE(Member(Member(template_response, "result"), "templates").as_array().empty());
}

// V-B2 (V2-4): three terminal outcomes; the domain carries no effect class.
TEST(CCLCOpenRequest, LifecycleStatusCarriesTheThreeOutcomesWithoutDomainEffect) {
  FakeOpenPort port;
  const ErrorDTO lost{"robot_controller_unresponsive", "no answer", {}};
  const ComponentLifecycleRequestedStateDTO active{
      ComponentLifecycleRequestedStatePresenceDTO::kResolved, ComponentLifecycleStateDTO::kActive};
  port.lifecycle_status = ComponentLifecycleStatusDTO{
      {RequestId("request.1"), RequestReplayStatusDTO::kAcceptedNew,
       LifecycleTransactionId("lifecycle.7"), LifecycleTransactionGeneration(7U),
       ComponentLifecycleGeneration(5U), 10, 1'000},
      {{RequestId("request.1"), RequestOwnerEpoch("owner.1"), "operator", std::nullopt},
       ControllableComponentId("component.arm"),
       ComponentLifecycleIntentKindDTO::kEnable,
       GraphRevision("graph.revision.1"),
       ComponentLifecycleGeneration(4U),
       {},
       std::nullopt},
      3U,
      RequestOwnerEpoch("owner.1"),
      ComponentLifecycleStateDTO::kPrepared,
      ComponentLifecycleStateDTO::kDeclared,
      active,
      ComponentLifecycleTransactionPhaseDTO::kFailed,
      {{ControlDomainId("domain.arm"), std::nullopt, std::nullopt, std::nullopt, std::nullopt,
        std::nullopt, active, ComponentLifecycleDomainActionStateDTO::kFailed, {}, lost}},
      false,
      ComponentLifecycleTerminalResultDTO::kConnectionLost,
      lost,
      10,
      20,
      0};
  const auto read = [&] {
    const auto response = ExecuteCanonicalCCLCOpenRequest(
        Request("lifecycle_status",
                CanonicalValue(CanonicalValue::Object{
                    {"lifecycle_transaction_id", CanonicalValue("lifecycle.7")}})),
        port, {});
    EXPECT_TRUE(response.ok());
    return Member(content_identity::parse_canonical_json(response.value()), "result");
  };
  auto result = read();
  EXPECT_EQ(Member(result, "terminal_result").as_string(), "connection_lost");
  EXPECT_EQ(Member(result, "phase").as_string(), "failed");
  const auto& row = Member(result, "domains").as_array().front();
  EXPECT_EQ(Member(row, "action_state").as_string(), "failed");
  EXPECT_EQ(row.find_object_member("effect"), nullptr);

  port.lifecycle_status->terminal_result = ComponentLifecycleTerminalResultDTO::kFailed;
  EXPECT_EQ(Member(read(), "terminal_result").as_string(), "failed");

  // A stopped Apply bring-up keeps its bookkeeping (VB2-O4 (a)).
  port.lifecycle_status->phase = ComponentLifecycleTransactionPhaseDTO::kOutcomeUnknown;
  port.lifecycle_status->terminal_result.reset();
  port.lifecycle_status->reconciliation_required = true;
  result = read();
  EXPECT_EQ(Member(result, "phase").as_string(), "outcome_unknown");
  EXPECT_TRUE(Member(result, "reconciliation_required").as_bool());
  EXPECT_TRUE(Member(result, "terminal_result").is_null());
}

TEST(CCLCOpenRequest, RejectsNoncanonicalUnknownAndPayloadAttributionFields) {
  FakeOpenPort port;
  for (const auto& request : {
           std::string(" {\"api_version\":\"manyforge.cclc_open_request.v1\"}"),
           Request("unknown", CanonicalValue(CanonicalValue::Object{})),
           Request("lifecycle_submit", CanonicalValue(CanonicalValue::Object{
                                           {"attribution", CanonicalValue("caller-asserted")}})),
       }) {
    const auto result = ExecuteCanonicalCCLCOpenRequest(request, port, {});
    ASSERT_TRUE(result.ok()) << result.error().message;
    const auto response = content_identity::parse_canonical_json(result.value());
    EXPECT_FALSE(Member(response, "ok").as_bool());
    EXPECT_EQ(Member(Member(response, "error"), "code").as_string(), "cclc_open_request_invalid");
  }
  EXPECT_FALSE(port.lifecycle_intent.has_value());
  EXPECT_FALSE(port.route_intent.has_value());
}

[[nodiscard]] std::string Sha(char value) { return "sha256:" + std::string(64U, value); }

[[nodiscard]] ComponentLifecycleCurrentSnapshotDTO VerifiedSnapshot() {
  const ProjectionRevisionDTO projection{RetainedBundleId("bundle.1"), GraphRevision("graph.1"),
                                         {SceneId(Sha('a')), 1U, ContentDigest(Sha('b'))}};
  const ExpectedControlBindingRevisionDTO binding{ControlBindingId("binding.arm"),
                                                  ControlBindingRevision("binding.rev.1"),
                                                  ControlRouteId("route.arm")};
  ComponentLifecycleCurrentSnapshotDTO snapshot{
      ContentDigest(Sha('0')),
      ControllableComponentId("component.arm"),
      ComponentLifecycleGeneration(1U),
      ComponentLifecycleStateDTO::kDeclared,
      {{NamedApplyPublicationTransactionDTO{ApplyId("apply.1"), ApplyGeneration(1U)}, projection,
        projection.retained_bundle_id, projection.graph_revision,
        ActiveBundlePublicationGeneration(1U), ContentDigest(Sha('c'))},
       ActiveBundlePublicationGeneration(2U),
       ContentDigest(Sha('d'))},
      {{ControlDomainId("domain.arm"), ControlDomainRevision("domain.rev.1"),
        ControllerTopologyId("topology.arm"), TopologyRevision("topology.rev.1"), std::nullopt,
        {binding}, {ResourceId("resource.arm")}}},
      {{ControlDomainId("domain.arm"), ControlDomainRevision("domain.rev.1"),
        ProviderInstanceId("provider.arm"), ProviderManifestId("manifest.arm"),
        ProviderManifestRevision(Sha('f')), RuntimeUnitId("runtime.arm"), RuntimeEpoch("epoch.1"),
        ContentDigest(Sha('e'))}}};
  snapshot.snapshot_digest = ComputeComponentLifecycleCurrentSnapshotDigest(snapshot).value();
  return snapshot;
}

[[nodiscard]] ComponentLifecycleReadinessDTO UnavailableReadiness(
    const ComponentLifecycleCurrentSnapshotDTO& snapshot) {
  const ErrorDTO unavailable{"cclc_product_read_unavailable", "test source has no qualified health", {}};
  ComponentLifecycleReadinessDTO value{RequestOwnerEpoch("owner"), 42,
      ComponentLifecycleIntentKindDTO::kPrepare, ComponentLifecycleStateDTO::kPrepared,
      false, unavailable, {}, {std::nullopt, std::nullopt, unavailable}};
  for (const auto& occurrence : snapshot.provider_occurrences)
    value.domains.push_back({occurrence, std::nullopt, std::nullopt, unavailable});
  return value;
}

class TargetViewPort final : public ICCLCOpenRequestPort {
 public:
  RequestOwnerEpoch ReadRequestOwnerEpoch() const override { return RequestOwnerEpoch("owner"); }
  Result<CCLCLifecycleRequestResultDTO> SubmitLifecycle(const ComponentLifecycleIntentDTO&) override {
    if (submit_result) return Result<CCLCLifecycleRequestResultDTO>::Ok(*submit_result);
    return Result<CCLCLifecycleRequestResultDTO>::Err({"unused", "unused", {}});
  }
  Result<std::optional<ComponentLifecycleStatusDTO>> LoadLifecycleStatus(
      const LifecycleTransactionId&) override {
    return Result<std::optional<ComponentLifecycleStatusDTO>>::Ok(std::nullopt);
  }
  Result<ComponentLifecycleCurrentSnapshotDTO> ReadCurrentComponent(
      const ControllableComponentId&) const override {
    ++legacy_component_reads;
    auto snapshot = VerifiedSnapshot();
    if (mutate_snapshot) mutate_snapshot(snapshot);
    return Result<ComponentLifecycleCurrentSnapshotDTO>::Ok(std::move(snapshot));
  }
  Result<CCLCComponentReadDTO> ReadCurrentComponentView(
      const ControllableComponentId&) const override {
    ++grouped_reads;
    auto snapshot = VerifiedSnapshot();
    CCLCComponentReadDTO result{snapshot, view, UnavailableReadiness(snapshot)};
    if (mutate_read) mutate_read(result);
    return Result<CCLCComponentReadDTO>::Ok(std::move(result));
  }
  Result<std::vector<ComponentIncidentEventDTO>> ReadIncidentEvents(
      std::uint64_t after, std::size_t limit) override {
    std::vector<ComponentIncidentEventDTO> result;
    for (const auto& event : events) {
      if (event.sequence > after && result.size() < limit) result.push_back(event);
    }
    return Result<std::vector<ComponentIncidentEventDTO>>::Ok(result);
  }
  Result<ComponentLifecycleTargetViewDTO> ReadLifecycleTargetView(
      const ControllableComponentId&) const override {
    ++legacy_target_reads;
    return Result<ComponentLifecycleTargetViewDTO>::Ok(view);
  }
  Result<CCLCRouteSelectionTemplateSetDTO> ReadRouteSelectionTemplates(
      const ControllableComponentId& id) const override {
    return Result<CCLCRouteSelectionTemplateSetDTO>::Ok(
        {id, RetainedBundleId("bundle.1"), GraphRevision("graph.1"), {}});
  }
  Result<CCLCRouteSelectionRequestResultDTO> SubmitRouteSelection(
      const RouteSelectionIntentDTO&) override {
    return Result<CCLCRouteSelectionRequestResultDTO>::Err({"unused", "unused", {}});
  }
  Result<std::optional<RouteSelectionStatusDTO>> LoadRouteSelectionStatus(
      const RouteSelectionTransactionId&) override {
    return Result<std::optional<RouteSelectionStatusDTO>>::Ok(std::nullopt);
  }

  mutable std::size_t grouped_reads{};
  mutable std::size_t legacy_component_reads{};
  mutable std::size_t legacy_target_reads{};
  std::function<void(CCLCComponentReadDTO&)> mutate_read;
  std::function<void(ComponentLifecycleCurrentSnapshotDTO&)> mutate_snapshot;
  ComponentLifecycleTargetViewDTO view;
  std::optional<CCLCLifecycleRequestResultDTO> submit_result;
  std::vector<ComponentIncidentEventDTO> events;
};

[[nodiscard]] CanonicalValue SnapshotResult(TargetViewPort& port) {
  const auto result = ExecuteCanonicalCCLCOpenRequest(
      Request("component_snapshot",
              CanonicalValue(CanonicalValue::Object{
                  {"controllable_component_id", CanonicalValue("component.arm")}})),
      port, {});
  if (!result.ok()) throw std::logic_error(result.error().message);
  const auto response = content_identity::parse_canonical_json(result.value());
  if (!Member(response, "ok").as_bool()) throw std::logic_error(result.value());
  return Member(response, "result");
}

[[nodiscard]] LifecycleDomainHealthDTO WireHealth() {
  return {ComponentLifecycleStateDTO::kPrepared,
          {{LifecycleFactId("boolean"), {LifecycleFactValueKindDTO::kBoolean, true, std::nullopt, std::nullopt}},
           {LifecycleFactId("integer"), {LifecycleFactValueKindDTO::kInteger, std::nullopt, 9007199254740991LL, std::nullopt}},
           {LifecycleFactId("token"), {LifecycleFactValueKindDTO::kToken, std::nullopt, std::nullopt, "automatic"}},
           {LifecycleFactId("unknown"), {LifecycleFactValueKindDTO::kUnknown, std::nullopt, std::nullopt, std::nullopt}}},
          std::nullopt};
}

void MakeReady(CCLCComponentReadDTO& value) {
  value.readiness.ready = true;
  value.readiness.error.reset();
  value.readiness.measured_state = {true, true, std::nullopt};
  for (auto& domain : value.readiness.domains) {
    domain.health = WireHealth();
    domain.decision_kind = LifecycleDomainPlanDecisionKind::kAlreadySatisfied;
    domain.error.reset();
  }
}

TEST(CCLCOpenRequest, ReadinessUsesOneGroupedReadAndDoesNotAlterVerifiedSnapshotIdentity) {
  TargetViewPort port;
  const auto first = SnapshotResult(port);
  EXPECT_EQ(port.grouped_reads, 1U);
  EXPECT_EQ(port.legacy_component_reads, 0U);
  EXPECT_EQ(port.legacy_target_reads, 0U);
  const auto& unavailable = Member(first, "readiness");
  EXPECT_EQ(unavailable.as_object().size(), 8U);
  EXPECT_FALSE(Member(unavailable, "ready").as_bool());
  EXPECT_EQ(Member(unavailable, "next_intent").as_string(), "prepare");
  EXPECT_EQ(Member(unavailable, "requested_state").as_string(), "prepared");
  EXPECT_EQ(Member(unavailable, "request_owner_epoch").as_string(), "owner");
  const auto digest = Member(first, "snapshot_digest").as_string();
  port.mutate_read = MakeReady;
  const auto ready = SnapshotResult(port);
  EXPECT_EQ(Member(ready, "snapshot_digest").as_string(), digest);
  EXPECT_TRUE(Member(Member(ready, "readiness"), "ready").as_bool());
  EXPECT_EQ(port.grouped_reads, 2U);
  EXPECT_EQ(port.legacy_component_reads, 0U);
  EXPECT_EQ(port.legacy_target_reads, 0U);
}

TEST(CCLCOpenRequest, ReadinessActivationStepIsOptionalBooleanOutsideHealthFacts) {
  TargetViewPort port;
  for (const auto metadata : {std::optional<bool>{}, std::optional<bool>{false},
                              std::optional<bool>{true}}) {
    port.mutate_read = [metadata](CCLCComponentReadDTO& value) {
      MakeReady(value);
      value.readiness.domains.front().has_activation_step = metadata;
      value.readiness.domains.front().health->has_activation_step = metadata;
    };
    const auto result = SnapshotResult(port);
    const auto& readiness = Member(result, "readiness");
    const auto& row = Member(readiness, "domains").as_array().front();
    EXPECT_TRUE(Member(readiness, "ready").as_bool());
    EXPECT_EQ(row.as_object().size(), metadata.has_value() ? 5U : 4U);
    if (metadata.has_value()) {
      EXPECT_EQ(Member(row, "has_activation_step").as_bool(), *metadata);
    }
    EXPECT_EQ(Member(row, "health").as_object().size(), 3U);
    EXPECT_EQ(Member(Member(row, "health"), "facts").as_array().size(), 4U);
  }
}

TEST(CCLCOpenRequest, ReadinessPreservesTaggedHealthFactsAndFullWidthEvaluationTime) {
  TargetViewPort port;
  port.mutate_read = [](CCLCComponentReadDTO& value) {
    MakeReady(value);
    value.readiness.evaluated_at_ns = std::numeric_limits<std::int64_t>::max();
  };
  const auto result = SnapshotResult(port);
  const auto& readiness = Member(result, "readiness");
  EXPECT_EQ(Member(readiness, "evaluated_at_ns").as_string(), "9223372036854775807");
  const auto& row = Member(readiness, "domains").as_array().front();
  EXPECT_EQ(Member(row, "provider_occurrence").as_object().size(), 8U);
  EXPECT_EQ(Member(row, "decision_kind").as_string(), "already_satisfied");
  const auto& health = Member(row, "health");
  EXPECT_EQ(Member(health, "observed_state").as_string(), "prepared");
  const auto& facts = Member(health, "facts").as_array();
  ASSERT_EQ(facts.size(), 4U);
  EXPECT_TRUE(Member(Member(facts[0], "value"), "boolean_value").as_bool());
  EXPECT_EQ(content_identity::canonical_json(Member(Member(facts[1], "value"), "integer_value")),
            "9007199254740991");
  EXPECT_EQ(Member(Member(facts[2], "value"), "token_value").as_string(), "automatic");
  EXPECT_EQ(Member(facts[3], "value").as_object().size(), 1U);
  port.mutate_read = [](CCLCComponentReadDTO& value) {
    MakeReady(value);
    value.readiness.domains.front().health->facts[1].value.integer_value = 9007199254740992LL;
  };
  EXPECT_THROW(static_cast<void>(SnapshotResult(port)), std::logic_error);
}

TEST(CCLCOpenRequest, LifecycleReadyPreservesUnavailableMeasuredStateWarning) {
  TargetViewPort port;
  port.mutate_read = [](CCLCComponentReadDTO& value) {
    MakeReady(value);
    value.readiness.measured_state = {
        true, false,
        ErrorDTO{"observation_stale", "Required measured state is stale", {}}};
  };
  const auto result = SnapshotResult(port);
  const auto& readiness = Member(result, "readiness");
  EXPECT_TRUE(Member(readiness, "ready").as_bool());
  const auto& measured = Member(readiness, "measured_state");
  EXPECT_TRUE(Member(measured, "required").as_bool());
  EXPECT_FALSE(Member(measured, "available").as_bool());
  EXPECT_EQ(Member(Member(measured, "error"), "code").as_string(),
            "observation_stale");
}

TEST(CCLCOpenRequest, ReadinessKeepsGeneralErrorsFullWhileHealthDiagnosticUsesLifecycleBound) {
  TargetViewPort port;
  ErrorDTO source_error{"measured_source_unavailable", "actual source error", {}};
  for (int i = 0; i < 64; ++i) source_error.details.emplace("source." + std::to_string(i), "actual");
  port.mutate_read = [&](CCLCComponentReadDTO& value) {
    value.readiness.error = source_error;
    value.readiness.measured_state = {true, false, source_error};
    auto& row = value.readiness.domains.front();
    row.error = source_error;
    row.health = WireHealth();
    row.health->diagnostic = ErrorDTO{"source_availability", "actual lifecycle diagnostic", {}};
    for (int i = 0; i < 8; ++i) row.health->diagnostic->details.emplace("fact." + std::to_string(i), "known");
  };
  const auto result = SnapshotResult(port);
  const auto& readiness = Member(result, "readiness");
  EXPECT_EQ(Member(Member(readiness, "error"), "details").as_object().size(), 64U);
  EXPECT_EQ(Member(Member(Member(readiness, "measured_state"), "error"), "details").as_object().size(), 64U);
  const auto& row = Member(readiness, "domains").as_array().front();
  EXPECT_EQ(Member(Member(row, "error"), "details").as_object().size(), 64U);
  EXPECT_EQ(Member(Member(Member(row, "health"), "diagnostic"), "details").as_object().size(), 8U);
  source_error.details.emplace("source.extra", "actual");
  EXPECT_THROW(static_cast<void>(SnapshotResult(port)), std::logic_error);
  source_error.details.erase("source.extra");
  port.mutate_read = [](CCLCComponentReadDTO& value) {
    auto& row = value.readiness.domains.front();
    row.health = WireHealth();
    row.health->diagnostic = ErrorDTO{"source_availability", "actual diagnostic", {}};
    for (int i = 0; i < 9; ++i) row.health->diagnostic->details.emplace("fact." + std::to_string(i), "known");
  };
  EXPECT_THROW(static_cast<void>(SnapshotResult(port)), std::logic_error);
}

TEST(CCLCOpenRequest, ActiveReadinessHasNoActivationIntentOrInventedDecision) {
  TargetViewPort port;
  port.mutate_read = [](CCLCComponentReadDTO& value) {
    value.snapshot.component_lifecycle_state = ComponentLifecycleStateDTO::kActive;
    value.snapshot.snapshot_digest = ComputeComponentLifecycleCurrentSnapshotDigest(value.snapshot).value();
    MakeReady(value);
    value.readiness.next_intent.reset();
    value.readiness.requested_state.reset();
    value.readiness.domains.front().decision_kind.reset();
    value.readiness.domains.front().health->observed_state = ComponentLifecycleStateDTO::kActive;
  };
  const auto result = SnapshotResult(port);
  const auto& readiness = Member(result, "readiness");
  EXPECT_TRUE(Member(readiness, "ready").as_bool());
  EXPECT_TRUE(Member(readiness, "next_intent").is_null());
  EXPECT_TRUE(Member(readiness, "requested_state").is_null());
  EXPECT_TRUE(Member(Member(readiness, "domains").as_array().front(), "decision_kind").is_null());
}

TEST(CCLCOpenRequest, ReadinessRejectsForeignOccurrenceAndContradictoryOwnerVerdicts) {
  const std::vector<std::function<void(CCLCComponentReadDTO&)>> mutations{
      [](auto& value) { value.readiness.request_owner_epoch = RequestOwnerEpoch(""); },
      [](auto& value) { value.readiness.evaluated_at_ns = -1; },
      [](auto& value) { value.readiness.domains.front().provider_occurrence.control_domain_revision = ControlDomainRevision("foreign"); },
      [](auto& value) { value.readiness.ready = true; value.readiness.error.reset(); },
      [](auto& value) { value.readiness.requested_state.reset(); },
      [](auto& value) { value.readiness.measured_state = {false, true, std::nullopt}; },
      [](auto& value) { value.readiness.measured_state = {true, true, value.readiness.error}; },
      [](auto& value) { value.readiness.domains.clear(); },
      [](auto& value) { MakeReady(value); value.readiness.domains.front().decision_kind =
          static_cast<LifecycleDomainPlanDecisionKind>(255); },
  };
  for (std::size_t i = 0; i < mutations.size(); ++i) {
    SCOPED_TRACE(i);
    TargetViewPort port;
    port.mutate_read = mutations[i];
    EXPECT_THROW(static_cast<void>(SnapshotResult(port)), std::logic_error);
  }
}

TEST(CCLCOpenRequest, ComponentSnapshotAlwaysCarriesNullableTargetAndProgress) {
  TargetViewPort port;
  const auto absent = SnapshotResult(port);
  EXPECT_TRUE(Member(absent, "lifecycle_target").is_null());
  EXPECT_TRUE(Member(absent, "lifecycle_progress").is_null());
  // The fields sit beside, not inside, the verified snapshot digest.
  const auto digest = Member(absent, "snapshot_digest").as_string();

  port.view = {ComponentLifecycleTargetDTO{ControllableComponentId("component.arm"),
                                           LifecycleTargetStateDTO::kActive,
                                           LifecycleTargetOriginDTO::kStartup, 1U},
               ComponentLifecycleProgressDTO{LifecycleProgressPhaseDTO::kWaitingForPrerequisites,
                                             ComponentLifecycleIntentKindDTO::kPrepare,
                                             LifecycleBlockingCauseClassDTO::kAvailability,
                                             std::nullopt, 5'000'000'000LL, true}};
  const auto present = SnapshotResult(port);
  EXPECT_EQ(Member(present, "snapshot_digest").as_string(), digest);
  const auto& target = Member(present, "lifecycle_target");
  EXPECT_EQ(Member(target, "requested_state").as_string(), "active");
  EXPECT_EQ(Member(target, "origin").as_string(), "startup");
  EXPECT_EQ(content_identity::canonical_json(Member(target, "target_generation")), "1");
  const auto& progress = Member(present, "lifecycle_progress");
  EXPECT_EQ(Member(progress, "phase").as_string(), "waiting_for_prerequisites");
  EXPECT_EQ(Member(progress, "next_intent").as_string(), "prepare");
  EXPECT_EQ(Member(progress, "blocking_cause_class").as_string(), "availability");
  EXPECT_TRUE(Member(progress, "blocking_reason").is_null());
  EXPECT_TRUE(Member(progress, "delayed").as_bool());
  EXPECT_EQ(content_identity::canonical_json(Member(progress, "waiting_since_ns")),
            "5000000000");
  EXPECT_TRUE(Member(progress, "incident").is_null());
  EXPECT_EQ(progress.as_object().size(), 7U);
}

TEST(CCLCOpenRequest, WaitingSinceUsesTheFullWidthClockContract) {
  TargetViewPort port;
  port.view = {ComponentLifecycleTargetDTO{ControllableComponentId("component.arm"),
                                           LifecycleTargetStateDTO::kActive,
                                           LifecycleTargetOriginDTO::kStartup, 1U},
               ComponentLifecycleProgressDTO{LifecycleProgressPhaseDTO::kWaitingForPrerequisites,
                                             ComponentLifecycleIntentKindDTO::kPrepare,
                                             LifecycleBlockingCauseClassDTO::kAvailability,
                                             std::nullopt, kBeyondSafeNs, true}};
  // Above 2^53-1 an owner-clock time is a canonical decimal string that
  // decodes back exactly; it is never refused or rounded.
  const auto wide = Member(SnapshotResult(port), "lifecycle_progress");
  EXPECT_EQ(content_identity::canonical_json(Member(wide, "waiting_since_ns")),
            "\"9007199254740993\"");
  EXPECT_EQ(content_identity::parse_nonnegative_int64(Member(wide, "waiting_since_ns")),
            kBeyondSafeNs);
  port.view.progress->waiting_since_ns = kMaximumClockNs;
  const auto maximum = Member(SnapshotResult(port), "lifecycle_progress");
  EXPECT_EQ(content_identity::canonical_json(Member(maximum, "waiting_since_ns")),
            "\"9223372036854775807\"");
  EXPECT_EQ(content_identity::parse_nonnegative_int64(Member(maximum, "waiting_since_ns")),
            kMaximumClockNs);

  // A negative owner time is refused, never clamped into a response.
  port.view.progress->waiting_since_ns = -1;
  EXPECT_THROW(static_cast<void>(SnapshotResult(port)), std::logic_error);
}

[[nodiscard]] ComponentLifecycleIncidentDTO OpenIncident() {
  return {"incident.component.arm.5000",
          ControllableComponentId("component.arm"),
          LifecycleBlockingCauseClassDTO::kEmergencyStop,
          {"C2", "mode=0", "state=4"},
          5'000,
          "Component component.arm reported an emergency stop.",
          std::nullopt,
          {},
          ComponentIncidentContainmentStateDTO::kRequested,
          ComponentIncidentContainmentReceiptDTO{StopId("stop.1"), StopGeneration(1U), {},
                                                 std::nullopt},
          std::nullopt,
          false,
          std::nullopt,
          std::nullopt,
          std::nullopt};
}

TEST(CCLCOpenRequest, ComponentOutputDropsAreAnOptionalInformationalFullWidthPair) {
  TargetViewPort port;
  EXPECT_EQ(SnapshotResult(port).find_object_member("dropped_output_lines"), nullptr);
  port.view.dropped_output_lines = 0U;
  port.view.dropped_output_bytes = std::numeric_limits<std::uint64_t>::max();
  const auto snapshot = SnapshotResult(port);
  EXPECT_EQ(Member(snapshot, "dropped_output_lines").as_number(), 0.0);
  EXPECT_EQ(Member(snapshot, "dropped_output_bytes").as_string(), "18446744073709551615");
  EXPECT_EQ(snapshot.find_object_member("lifecycle_progress")->is_null(), true);
  port.view.dropped_output_bytes.reset();
  EXPECT_THROW(static_cast<void>(SnapshotResult(port)), std::logic_error);
}

TEST(CCLCOpenRequest, IncidentRetiredActiveTargetIsShownOnlyWhileTrue) {
  TargetViewPort port;
  EXPECT_EQ(SnapshotResult(port).find_object_member("active_target_retired_by_incident"), nullptr);
  port.view.active_target_retired_by_incident = true;
  EXPECT_TRUE(Member(SnapshotResult(port), "active_target_retired_by_incident").as_bool());
}

TEST(CCLCOpenRequest, ComponentSnapshotShowsTheOpenIncidentInContractShape) {
  TargetViewPort port;
  port.view = {ComponentLifecycleTargetDTO{ControllableComponentId("component.arm"),
                                           LifecycleTargetStateDTO::kActive,
                                           LifecycleTargetOriginDTO::kStartup, 1U},
               ComponentLifecycleProgressDTO{LifecycleProgressPhaseDTO::kBlocked,
                                             ComponentLifecycleIntentKindDTO::kRecover,
                                             LifecycleBlockingCauseClassDTO::kEmergencyStop,
                                             std::string("incident_open:emergency_stop"), 5'000,
                                             false, OpenIncident()}};
  const auto progress = Member(SnapshotResult(port), "lifecycle_progress");
  EXPECT_EQ(Member(progress, "phase").as_string(), "blocked");
  EXPECT_EQ(Member(progress, "blocking_cause_class").as_string(), "emergency_stop");
  const auto& incident = Member(progress, "incident");
  EXPECT_EQ(
      content_identity::canonical_json(incident),
      R"({"acknowledged":false,"cause_class":"emergency_stop","detected_at_ns":5000,"diagnostic":null,)"
      R"("incident_id":"incident.component.arm.5000","summary":"Component component.arm )"
      R"(reported an emergency stop.","vendor_codes":["C2","mode=0","state=4"]})");

  // Full-width clock: above 2^53-1 the time is a canonical decimal string.
  auto wide = OpenIncident();
  wide.detected_at_ns = kBeyondSafeNs;
  port.view.progress->incident = wide;
  const auto wide_incident =
      Member(Member(SnapshotResult(port), "lifecycle_progress"), "incident");
  EXPECT_EQ(content_identity::canonical_json(Member(wide_incident, "detected_at_ns")),
            "\"9007199254740993\"");
  EXPECT_EQ(content_identity::parse_nonnegative_int64(Member(wide_incident, "detected_at_ns")),
            kBeyondSafeNs);

  // A negative detection time is refused, never clamped.
  auto negative = OpenIncident();
  negative.detected_at_ns = -1;
  port.view.progress->incident = negative;
  EXPECT_THROW(static_cast<void>(SnapshotResult(port)), std::logic_error);
}

TEST(CCLCOpenRequest, IncidentRecoveryResultAndIncidentEventsAreExact) {
  TargetViewPort port;
  CCLCLifecycleRequestResultDTO recovered{RequestReplayStatusDTO::kAcceptedNew, std::nullopt,
                                          std::nullopt, std::nullopt};
  recovered.incident_recovery = ComponentIncidentRecoveryDTO{
      "incident.component.arm.5000", ControllableComponentId("component.arm"),
      LifecycleBlockingCauseClassDTO::kEmergencyStop, "operator.ada", 6'000};
  ASSERT_TRUE(recovered.locally_valid());
  port.submit_result = recovered;
  const auto submitted = ExecuteCanonicalCCLCOpenRequest(
      Request("lifecycle_submit", LifecycleIntentValue()), port, {});
  ASSERT_TRUE(submitted.ok()) << submitted.error().message;
  const auto response = content_identity::parse_canonical_json(submitted.value());
  ASSERT_TRUE(Member(response, "ok").as_bool()) << submitted.value();
  const auto& result = Member(response, "result");
  EXPECT_TRUE(Member(result, "acceptance").is_null());
  EXPECT_TRUE(Member(result, "status").is_null());
  EXPECT_EQ(Member(result, "replay_status").as_string(), "accepted_new");
  EXPECT_EQ(content_identity::canonical_json(Member(result, "incident_recovery")),
            R"({"acknowledged_at_ns":6000,"acknowledged_by":"operator.ada",)"
            R"("cause_class":"emergency_stop","controllable_component_id":"component.arm",)"
            R"("incident_id":"incident.component.arm.5000"})");
  recovered.incident_recovery->closure_reason =
      ComponentIncidentClosureReasonDTO::kCellClosedByOperator;
  port.submit_result = recovered;
  const auto close_wire = ExecuteCanonicalCCLCOpenRequest(
      Request("lifecycle_submit", LifecycleIntentValue()), port, {});
  ASSERT_TRUE(close_wire.ok());
  const auto close_response = content_identity::parse_canonical_json(close_wire.value());
  EXPECT_EQ(Member(Member(Member(close_response, "result"), "incident_recovery"),
                   "closure_reason").as_string(), "cell_closed_by_operator");
  // An incident recovery never carries a transaction or a rejection.
  auto mixed = recovered;
  mixed.rejection = ErrorDTO{"x", "x", {{"effect", "not_started"}}};
  EXPECT_FALSE(mixed.locally_valid());

  port.events = {{1U, "component_incident", "incident.component.arm.5000", 5'000,
                  R"({"event_kind":"component_incident"})"},
                 {2U, "component_incident_acknowledged", "incident.component.arm.5000", 6'000,
                  R"({"event_kind":"component_incident_acknowledged"})"}};
  const auto read = ExecuteCanonicalCCLCOpenRequest(
      Request("incident_events", CanonicalValue(CanonicalValue::Object{
                                     {"after_sequence", CanonicalValue(1.0)},
                                     {"limit", CanonicalValue(16.0)}})),
      port, {});
  ASSERT_TRUE(read.ok());
  const auto events = Member(content_identity::parse_canonical_json(read.value()), "result");
  EXPECT_EQ(content_identity::canonical_json(events),
            R"({"events":[{"event_kind":"component_incident_acknowledged","event_sequence":2,)"
            R"("incident_id":"incident.component.arm.5000","occurred_at_ns":6000,)"
            R"("payload":{"event_kind":"component_incident_acknowledged"}}],)"
            R"("through_sequence":2})");
  const auto unbounded = ExecuteCanonicalCCLCOpenRequest(
      Request("incident_events", CanonicalValue(CanonicalValue::Object{
                                     {"after_sequence", CanonicalValue(0.0)},
                                     {"limit", CanonicalValue(65.0)}})),
      port, {});
  ASSERT_TRUE(unbounded.ok());
  EXPECT_FALSE(Member(content_identity::parse_canonical_json(unbounded.value()), "ok").as_bool());
}

TEST(CCLCOpenRequest, NearMaximumIncidentEventsPageWithinTheExistingByteBoundAndAdvanceExactly) {
  TargetViewPort port;
  // Quoting costs two encoded bytes per source byte. Each whole payload is
  // near the approved512KiB row bound, so a64-row page cannot fit8MiB.
  const auto payload = content_identity::canonical_json(CanonicalValue(CanonicalValue::Object{
      {"tail", CanonicalValue(std::string((kMaxComponentIncidentRecordBytes - 16U) / 2U, '"'))}}));
  ASSERT_LE(payload.size(), kMaxComponentIncidentRecordBytes);
  ASSERT_GT(payload.size(), kMaxComponentIncidentRecordBytes - 32U);
  for (std::uint64_t sequence = 1U; sequence <= kMaxComponentIncidentEventPage; ++sequence)
    port.events.push_back(
        {sequence, "component_incident", "incident.arm", kMaximumClockNs, payload});
  std::uint64_t after{};
  std::size_t pages{};
  while (after < port.events.size()) {
    const auto read = ExecuteCanonicalCCLCOpenRequest(
        Request(
            "incident_events",
            CanonicalValue(CanonicalValue::Object{
                {"after_sequence", CanonicalValue(static_cast<double>(after))},
                {"limit", CanonicalValue(static_cast<double>(kMaxComponentIncidentEventPage))}})),
        port, {});
    ASSERT_TRUE(read.ok()) << read.error().message;
    EXPECT_LE(read.value().size(), kMaximumCCLCOpenResponseBytes);
    const auto response = content_identity::parse_canonical_json(read.value());
    ASSERT_TRUE(Member(response, "ok").as_bool());
    const auto& result = Member(response, "result");
    const auto& events = Member(result, "events").as_array();
    ASSERT_FALSE(events.empty());
    EXPECT_LT(events.size(), kMaxComponentIncidentEventPage);
    for (const auto& event : events) {
      EXPECT_EQ(Member(event, "event_sequence").as_number(), static_cast<double>(++after));
      EXPECT_EQ(content_identity::canonical_json(Member(event, "payload")), payload);
    }
    EXPECT_EQ(Member(result, "through_sequence").as_number(), static_cast<double>(after));
    ASSERT_LE(++pages, kMaxComponentIncidentEventPage);
  }
  EXPECT_EQ(after, kMaxComponentIncidentEventPage);
  EXPECT_GT(pages, 1U);
}

TEST(CCLCOpenRequest, IncidentRecoveryAndEventTimesUseTheFullWidthClockContract) {
  TargetViewPort port;
  CCLCLifecycleRequestResultDTO recovered{RequestReplayStatusDTO::kAcceptedNew, std::nullopt,
                                          std::nullopt, std::nullopt};
  recovered.incident_recovery = ComponentIncidentRecoveryDTO{
      "incident.component.arm.5000", ControllableComponentId("component.arm"),
      LifecycleBlockingCauseClassDTO::kEmergencyStop, "operator.ada", kMaximumClockNs};
  port.submit_result = recovered;
  const auto submitted = ExecuteCanonicalCCLCOpenRequest(
      Request("lifecycle_submit", LifecycleIntentValue()), port, {});
  ASSERT_TRUE(submitted.ok()) << submitted.error().message;
  const auto response = content_identity::parse_canonical_json(submitted.value());
  ASSERT_TRUE(Member(response, "ok").as_bool()) << submitted.value();
  const auto& acknowledged_at =
      Member(Member(Member(response, "result"), "incident_recovery"), "acknowledged_at_ns");
  EXPECT_EQ(content_identity::canonical_json(acknowledged_at), "\"9223372036854775807\"");
  EXPECT_EQ(content_identity::parse_nonnegative_int64(acknowledged_at), kMaximumClockNs);

  // A negative acknowledgement time is not a valid result.
  recovered.incident_recovery->acknowledged_at_ns = -1;
  port.submit_result = recovered;
  const auto negative = ExecuteCanonicalCCLCOpenRequest(
      Request("lifecycle_submit", LifecycleIntentValue()), port, {});
  ASSERT_TRUE(negative.ok());
  EXPECT_FALSE(Member(content_identity::parse_canonical_json(negative.value()), "ok").as_bool());

  port.events = {{1U, "component_incident_acknowledged", "incident.component.arm.5000",
                  kBeyondSafeNs, R"({"event_kind":"component_incident_acknowledged"})"}};
  const auto read = ExecuteCanonicalCCLCOpenRequest(
      Request("incident_events", CanonicalValue(CanonicalValue::Object{
                                     {"after_sequence", CanonicalValue(0.0)},
                                     {"limit", CanonicalValue(16.0)}})),
      port, {});
  ASSERT_TRUE(read.ok()) << read.error().message;
  const auto events = Member(content_identity::parse_canonical_json(read.value()), "result");
  const auto& occurred = Member(Member(events, "events").as_array().front(), "occurred_at_ns");
  EXPECT_EQ(content_identity::canonical_json(occurred), "\"9007199254740993\"");
  EXPECT_EQ(content_identity::parse_nonnegative_int64(occurred), kBeyondSafeNs);

  // A negative event time is an owner failure, never clamped to zero.
  port.events.front().occurred_at_ns = -1;
  const auto refused = ExecuteCanonicalCCLCOpenRequest(
      Request("incident_events", CanonicalValue(CanonicalValue::Object{
                                     {"after_sequence", CanonicalValue(0.0)},
                                     {"limit", CanonicalValue(16.0)}})),
      port, {});
  EXPECT_FALSE(refused.ok());
}

TEST(CCLCOpenRequest, TargetRetirementEchoesCompleteClosedCASAndSettlementNeverHidesUnknown) {
  FakeOpenPort port;
  const RequestAttributionDTO attribution{RequestTransportDTO::kHttp, RequestAssuranceDTO::kUnverified,
                                          std::nullopt};
  const auto offer = ExecuteCanonicalCCLCOpenRequest(
      Request("lifecycle_target_retirement_offer", CanonicalValue(CanonicalValue::Object{})), port, attribution);
  ASSERT_TRUE(offer.ok());
  const auto offered = content_identity::parse_canonical_json(offer.value());
  ASSERT_TRUE(Member(offered, "ok").as_bool());
  const auto scope = Member(Member(offered, "result"), "scope");
  const auto targets = Member(Member(offered, "result"), "targets");
  ASSERT_EQ(targets.as_array().size(), 1U);
  EXPECT_FALSE(Member(targets.as_array().front(), "reached").as_bool());
  EXPECT_EQ(Member(targets.as_array().front(), "recovered_cause").as_string(), "fault");
  const CanonicalValue intent(CanonicalValue::Object{
      {"request_context", ContextValue("retire.1")}, {"scope", scope}, {"expected_targets", targets}});
  const auto retired = ExecuteCanonicalCCLCOpenRequest(Request("lifecycle_target_retire", intent), port, attribution);
  ASSERT_TRUE(retired.ok());
  const auto result = content_identity::parse_canonical_json(retired.value());
  EXPECT_TRUE(Member(result, "ok").as_bool());
  EXPECT_EQ(Member(Member(result, "result"), "retired_targets"), targets);
  EXPECT_EQ(Member(Member(result, "result"), "request_context"), ContextValue("retire.1"));
  ASSERT_TRUE(port.retirement_request);
  EXPECT_EQ(port.retirement_request->expected_targets, port.retirement_rows);
  const auto read = ExecuteCanonicalCCLCOpenRequest(
      Request("lifecycle_target_retirement_result", intent), port, attribution);
  ASSERT_TRUE(read.ok());
  EXPECT_TRUE(Member(content_identity::parse_canonical_json(read.value()), "result").is_null());
  EXPECT_EQ(port.retirement_read, port.retirement_request);
  EXPECT_EQ(port.retirement_writes, 1);
  const CanonicalValue settlement(CanonicalValue::Object{{"scope", scope}});
  const auto unknown = ExecuteCanonicalCCLCOpenRequest(
      Request("lifecycle_owner_settlement", settlement), port, attribution);
  ASSERT_TRUE(unknown.ok());
  EXPECT_FALSE(Member(content_identity::parse_canonical_json(unknown.value()), "ok").as_bool());
  port.settlement_unknown = false;
  const auto settled = ExecuteCanonicalCCLCOpenRequest(
      Request("lifecycle_owner_settlement", settlement), port, attribution);
  ASSERT_TRUE(settled.ok());
  EXPECT_TRUE(Member(Member(content_identity::parse_canonical_json(settled.value()), "result"), "settled").as_bool());
  EXPECT_EQ(port.retirement_writes, 1);

  auto bad_fields = intent.as_object();
  bad_fields.emplace_back("confirmed", CanonicalValue(true));
  const auto bad = ExecuteCanonicalCCLCOpenRequest(
      Request("lifecycle_target_retire", CanonicalValue(std::move(bad_fields))), port, attribution);
  ASSERT_TRUE(bad.ok());
  EXPECT_FALSE(Member(content_identity::parse_canonical_json(bad.value()), "ok").as_bool());
  auto duplicate = targets.as_array();
  duplicate.push_back(duplicate.front());
  const auto bad_rows = ExecuteCanonicalCCLCOpenRequest(Request("lifecycle_target_retire",
      CanonicalValue(CanonicalValue::Object{{"request_context", ContextValue("retire.2")},
         {"scope", scope}, {"expected_targets", CanonicalValue(std::move(duplicate))}})), port, attribution);
  ASSERT_TRUE(bad_rows.ok());
  EXPECT_FALSE(Member(content_identity::parse_canonical_json(bad_rows.value()), "ok").as_bool());
  EXPECT_EQ(port.retirement_writes, 1);
}

}  // namespace
}  // namespace manyforge::core

namespace manyforge::core {
namespace {
TEST(CCLCOpenRequest, ProgramLifecyclePointReadDoesNotCollectChangingReadiness) {
  TargetViewPort port;
  port.mutate_read = [](CCLCComponentReadDTO&) {
    throw std::logic_error("background admission publication changed during full readiness read");
  };
  const auto result = ExecuteCanonicalCCLCOpenRequest(
      Request("component_lifecycle_snapshot", CanonicalValue(CanonicalValue::Object{
          {"controllable_component_id", CanonicalValue("component.arm")}})), port, {});
  ASSERT_TRUE(result.ok());
  const auto response = content_identity::parse_canonical_json(result.value());
  ASSERT_TRUE(Member(response, "ok").as_bool()) << result.value();
  const auto& snapshot = Member(response, "result");
  EXPECT_EQ(snapshot.as_object().size(), 10U);
  EXPECT_EQ(Member(snapshot, "controllable_component_id").as_string(), "component.arm");
  EXPECT_EQ(Member(snapshot, "component_lifecycle_state").as_string(), "declared");
  EXPECT_EQ(Member(snapshot, "snapshot_digest").as_string(), VerifiedSnapshot().snapshot_digest.value());
  EXPECT_EQ(snapshot.find_object_member("readiness"), nullptr);
  EXPECT_EQ(snapshot.find_object_member("lifecycle_target"), nullptr);
  EXPECT_EQ(port.legacy_component_reads, 1U);
  EXPECT_EQ(port.grouped_reads, 0U);
  EXPECT_EQ(port.legacy_target_reads, 0U);
  EXPECT_FALSE(port.submit_result.has_value());
}

TEST(CCLCOpenRequest, ProgramLifecyclePointReadRejectsCorruptSnapshotAndOpenPayload) {
  TargetViewPort port;
  port.mutate_snapshot = [](ComponentLifecycleCurrentSnapshotDTO& value) {
    value.component_lifecycle_state = ComponentLifecycleStateDTO::kActive;
    // A changed lifecycle state with the predecessor digest is not a verified snapshot.
  };
  const auto result = ExecuteCanonicalCCLCOpenRequest(
      Request("component_lifecycle_snapshot", CanonicalValue(CanonicalValue::Object{
          {"controllable_component_id", CanonicalValue("component.arm")}})), port, {});
  ASSERT_TRUE(result.ok());
  EXPECT_FALSE(Member(content_identity::parse_canonical_json(result.value()), "ok").as_bool());
  const auto closed = ExecuteCanonicalCCLCOpenRequest(
      Request("component_lifecycle_snapshot", CanonicalValue(CanonicalValue::Object{
          {"controllable_component_id", CanonicalValue("component.arm")},
          {"ignore_readiness", CanonicalValue(true)}})), port, {});
  ASSERT_TRUE(closed.ok());
  EXPECT_FALSE(Member(content_identity::parse_canonical_json(closed.value()), "ok").as_bool());
  EXPECT_EQ(port.legacy_component_reads, 1U);
  EXPECT_EQ(port.grouped_reads, 0U);
}

TEST(CCLCOpenRequest, ProviderSnapshotUsesTypedScopeAndExactSafeNumericFields) {
  TargetViewPort port;
  port.mutate_snapshot = [](ComponentLifecycleCurrentSnapshotDTO &value) {
    const auto occurrence = value.provider_occurrences.front();
    const ProviderControlDomainDTO provider{
        value.controllable_component_id,
        occurrence.provider_runtime_unit_id,
        occurrence.provider_instance_id,
        occurrence.provider_manifest_id,
        occurrence.provider_manifest_revision,
        occurrence.lifecycle_plan_catalog_digest,
        ContentDigest(Sha('1')),
        {"ready"},
        30'000'000'000LL};
    value.domains = {
        {occurrence.control_domain_id, occurrence.control_domain_revision,
         ResolvedProviderDomainScopeDTO{
             provider,
             occurrence.provider_runtime_epoch,
             ProviderDomainActiveConfigurationDTO{
                 ActiveControlConfigurationId("provider.configuration"),
                 ActiveControlConfigurationGeneration(9'007'199'254'740'991ULL),
                 occurrence.provider_runtime_unit_id,
                 occurrence.provider_runtime_epoch, 9'007'199'254'740'991ULL,
                 provider.signal_configuration_digest},
             {}}}};
    const auto digest = ComputeComponentLifecycleCurrentSnapshotDigest(value);
    if (!digest.ok())
      throw std::logic_error(digest.error().message);
    value.snapshot_digest = digest.value();
  };
  const auto result = ExecuteCanonicalCCLCOpenRequest(
      Request(
          "component_lifecycle_snapshot",
          CanonicalValue(CanonicalValue::Object{
              {"controllable_component_id", CanonicalValue("component.arm")}})),
      port, {});
  ASSERT_TRUE(result.ok());
  const auto response = content_identity::parse_canonical_json(result.value());
  ASSERT_TRUE(Member(response, "ok").as_bool()) << result.value();
  const auto &domain =
      Member(Member(response, "result"), "domains").as_array().front();
  EXPECT_EQ(Member(domain, "kind").as_string(), "io_provider");
  EXPECT_EQ(domain.find_object_member("controller_topology_id"), nullptr);
  EXPECT_EQ(Member(Member(domain, "provider"), "lifecycle_command_timeout_ns")
                .as_number(),
            30'000'000'000.0);
  const auto &configuration = Member(domain, "active_control_configuration");
  EXPECT_EQ(Member(configuration, "active_control_configuration_generation")
                .as_number(),
            9'007'199'254'740'991.0);
  EXPECT_EQ(Member(configuration, "source_sequence").as_number(),
            9'007'199'254'740'991.0);
  EXPECT_EQ(port.grouped_reads, 0U);
}
// Fixed pre-wire-cutover identities: these snapshots retain their canonical
// identity keys while both public read commands use the common resources field.
TEST(CCLCOpenRequest, DomainResourcesAreCommonAndDoNotChangeSnapshotIdentity) {
  for (const bool provider : {false, true}) {
    for (const bool has_output : {false, true}) {
      if (!provider && !has_output) continue;
      SCOPED_TRACE(provider);
      SCOPED_TRACE(has_output);
      auto snapshot = VerifiedSnapshot();
      if (provider) {
        const auto occurrence = snapshot.provider_occurrences.front();
        const ProviderControlDomainDTO proof{
            snapshot.controllable_component_id, occurrence.provider_runtime_unit_id,
            occurrence.provider_instance_id, occurrence.provider_manifest_id,
            occurrence.provider_manifest_revision, occurrence.lifecycle_plan_catalog_digest,
            ContentDigest(Sha('1')), {"ready"}, 30'000'000'000LL};
        snapshot.domains = {{occurrence.control_domain_id, occurrence.control_domain_revision,
                            ResolvedProviderDomainScopeDTO{
                                proof, occurrence.provider_runtime_epoch, std::nullopt,
                                has_output ? std::vector<ResourceId>{ResourceId("resource.arm")}
                                           : std::vector<ResourceId>{}}}};
      }
      const std::string expected_digest = !provider
          ? "sha256:9b17871d075ae1b5dfd78a210160d8bed4781a76d331e56251c10485dac57f16"
          : has_output
              ? "sha256:6b9e58230f9fc6bd78dbc48dbeebd918fe8134171f118743cf3c164d904f1950"
              : "sha256:84a356db1fc9cd458e1a652b25cba171f912e0286ffb841201c85ebf47a8ca27";
      const auto digest = ComputeComponentLifecycleCurrentSnapshotDigest(snapshot);
      ASSERT_TRUE(digest.ok()) << digest.error().message;
      ASSERT_EQ(digest.value().value(), expected_digest);
      snapshot.snapshot_digest = digest.value();
      TargetViewPort port;
      port.mutate_snapshot = [&](auto& value) { value = snapshot; };
      port.mutate_read = [&](auto& value) {
        value.snapshot = snapshot;
        value.readiness = UnavailableReadiness(snapshot);
      };
      for (const std::string command : {"component_lifecycle_snapshot", "component_snapshot"}) {
        const auto encoded = ExecuteCanonicalCCLCOpenRequest(
            Request(command, CanonicalValue(CanonicalValue::Object{
                {"controllable_component_id", CanonicalValue("component.arm")}})), port, {});
        ASSERT_TRUE(encoded.ok()) << encoded.error().message;
        const auto response = content_identity::parse_canonical_json(encoded.value());
        ASSERT_TRUE(Member(response, "ok").as_bool()) << encoded.value();
        const auto& value = Member(response, "result");
        const auto& domain = Member(value, "domains").as_array().front();
        EXPECT_EQ(Member(domain, "kind").as_string(), provider ? "io_provider" : "controller");
        EXPECT_EQ(domain.find_object_member("current_resources"), nullptr);
        EXPECT_EQ(domain.find_object_member("output_resources"), nullptr);
        const auto& resources = Member(domain, "resources").as_array();
        EXPECT_EQ(resources.size(), has_output ? 1U : 0U);
        if (has_output) {
          EXPECT_EQ(resources.front().as_string(), "resource.arm");
        }
        EXPECT_EQ(Member(value, "snapshot_digest").as_string(), expected_digest);
      }
      EXPECT_TRUE(VerifyComponentLifecycleCurrentSnapshot(snapshot).ok());
    }
  }
}

TEST(CCLCOpenRequest, ExpectedDomainsRequireExplicitKindWithoutResourceEcho) {
  // The provider request echoes only the existing expected-scope fields from
  // a real typed snapshot. Resources/runtime epoch remain response facts.
  TargetViewPort snapshot_port;
  snapshot_port.mutate_snapshot = [](auto& snapshot) {
    const auto occurrence = snapshot.provider_occurrences.front();
    const ProviderControlDomainDTO proof{
        snapshot.controllable_component_id, occurrence.provider_runtime_unit_id,
        occurrence.provider_instance_id, occurrence.provider_manifest_id,
        occurrence.provider_manifest_revision, occurrence.lifecycle_plan_catalog_digest,
        ContentDigest(Sha('1')), {"ready"}, 30'000'000'000LL};
    snapshot.domains = {{occurrence.control_domain_id, occurrence.control_domain_revision,
                        ResolvedProviderDomainScopeDTO{
                            proof, occurrence.provider_runtime_epoch, std::nullopt, {}}}};
    snapshot.snapshot_digest = ComputeComponentLifecycleCurrentSnapshotDigest(snapshot).value();
  };
  const auto encoded_snapshot = ExecuteCanonicalCCLCOpenRequest(
      Request("component_lifecycle_snapshot", CanonicalValue(CanonicalValue::Object{
          {"controllable_component_id", CanonicalValue("component.arm")}})), snapshot_port, {});
  ASSERT_TRUE(encoded_snapshot.ok());
  const auto response = content_identity::parse_canonical_json(encoded_snapshot.value());
  ASSERT_TRUE(Member(response, "ok").as_bool()) << encoded_snapshot.value();
  const auto& actual = Member(Member(response, "result"), "domains").as_array().front();
  const CanonicalValue provider_expected(CanonicalValue::Object{
      {"kind", Member(actual, "kind")},
      {"control_domain_id", Member(actual, "control_domain_id")},
      {"control_domain_revision", Member(actual, "control_domain_revision")},
      {"provider", Member(actual, "provider")},
      {"expected_active_control_configuration",
       CanonicalValue(CanonicalValue::Object{{"presence", CanonicalValue("absent")}})}});
  const auto controller_expected = Member(LifecycleIntentValue(), "expected_control_domains").as_array().front();
  const auto request_for = [](const CanonicalValue& domain) {
    auto fields = LifecycleIntentValue().as_object();
    for (auto& [name, value] : fields)
      if (name == "expected_control_domains")
        value = CanonicalValue(CanonicalValue::Array{domain});
    return Request("lifecycle_submit", CanonicalValue(std::move(fields)));
  };
  for (const auto& expected : {controller_expected, provider_expected}) {
    FakeOpenPort port;
    const auto accepted = ExecuteCanonicalCCLCOpenRequest(request_for(expected), port, {});
    ASSERT_TRUE(accepted.ok());
    EXPECT_TRUE(Member(content_identity::parse_canonical_json(accepted.value()), "ok").as_bool());
    ASSERT_TRUE(port.lifecycle_intent.has_value());
    EXPECT_EQ(port.lifecycle_intent->expected_control_domains.front().provider() != nullptr,
              Member(expected, "kind").as_string() == "io_provider");
    // Missing/old/unknown kinds and response-only resource fields are hard
    // refusals before the existing lifecycle port is called.
    for (const unsigned variant : {0U, 1U, 2U, 3U, 4U, 5U}) {
      auto fields = expected.as_object();
      if (variant == 0U) {
        std::erase_if(fields, [](const auto& field) { return field.first == "kind"; });
      } else if (variant <= 2U) {
        for (auto& [name, value] : fields)
          if (name == "kind") value = CanonicalValue(variant == 1U ? "provider" : "unknown");
      } else {
        fields.emplace_back(variant == 3U ? "resources" : variant == 4U ? "current_resources" : "output_resources",
                            CanonicalValue(CanonicalValue::Array{}));
      }
      port.lifecycle_intent.reset();
      const auto rejected = ExecuteCanonicalCCLCOpenRequest(
          request_for(CanonicalValue(std::move(fields))), port, {});
      ASSERT_TRUE(rejected.ok());
      const auto refusal = content_identity::parse_canonical_json(rejected.value());
      EXPECT_FALSE(Member(refusal, "ok").as_bool());
      EXPECT_EQ(Member(Member(refusal, "error"), "code").as_string(), "cclc_open_request_invalid");
      EXPECT_FALSE(port.lifecycle_intent.has_value());
    }
  }
}

}  // namespace
}  // namespace manyforge::core
