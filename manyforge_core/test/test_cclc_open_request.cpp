#include <gtest/gtest.h>

#include <manyforge_content_identity/content_identity.hpp>
#include <manyforge_core/cclc_open_request.hpp>
#include <optional>
#include <string>
#include <utility>

namespace manyforge::core {
namespace {

using content_identity::CanonicalValue;

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

  Result<std::optional<ComponentLifecycleStatusDTO>> LoadLifecycleStatus(
      const LifecycleTransactionId& transaction_id) override {
    lifecycle_status_id = transaction_id;
    return Result<std::optional<ComponentLifecycleStatusDTO>>::Ok(std::nullopt);
  }

  Result<ComponentLifecycleCurrentSnapshotDTO> ReadCurrentComponent(
      const ControllableComponentId& component_id) const override {
    component_id_read = component_id;
    return Result<ComponentLifecycleCurrentSnapshotDTO>::Err(
        {"missing_reference", "component snapshot is absent", {}});
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

  std::optional<ComponentLifecycleIntentDTO> lifecycle_intent;
  std::optional<RouteSelectionIntentDTO> route_intent;
  std::optional<RouteSelectionIntentDTO> route_result_intent;
  std::optional<LifecycleTransactionId> lifecycle_status_id;
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
    return Result<ComponentLifecycleCurrentSnapshotDTO>::Ok(VerifiedSnapshot());
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

  // Owner-clock times stay I-JSON safe integers; a larger value never reaches
  // a response, because Headless rejects it.
  port.view.progress->waiting_since_ns = 1'700'000'000'000'000'000LL;
  EXPECT_ANY_THROW(static_cast<void>(SnapshotResult(port)));
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
  EXPECT_EQ(content_identity::canonical_json(incident),
            R"({"acknowledged":false,"cause_class":"emergency_stop","detected_at_ns":5000,)"
            R"("incident_id":"incident.component.arm.5000","summary":"Component component.arm )"
            R"(reported an emergency stop.","vendor_codes":["C2","mode=0","state=4"]})");

  // Incident times beyond an I-JSON safe integer never reach a response.
  auto unsafe = OpenIncident();
  unsafe.detected_at_ns = 1'700'000'000'000'000'000LL;
  port.view.progress->incident = unsafe;
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

}  // namespace
}  // namespace manyforge::core
