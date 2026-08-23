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

[[nodiscard]] CanonicalValue RouteIntentValue() {
  return CanonicalValue(CanonicalValue::Object{
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
      {"request_context", ContextValue("request.route.1")}});
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

  std::optional<ComponentLifecycleIntentDTO> lifecycle_intent;
  std::optional<RouteSelectionIntentDTO> route_intent;
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
  EXPECT_EQ(port.route_intent->attribution.transport, RequestTransportDTO::kRos);
  EXPECT_EQ(port.route_intent->desired_configuration.control_domain_id,
            ControlDomainId("domain.arm"));
  ASSERT_EQ(port.route_intent->desired_configuration.binding_routes.size(), 1U);

  const auto response = content_identity::parse_canonical_json(result.value());
  EXPECT_TRUE(Member(response, "ok").as_bool());
  EXPECT_EQ(Member(Member(response, "result"), "replay_status").as_string(), "request_ledger_full");
}

TEST(CCLCOpenRequest, StatusAndSnapshotReadsCarryNoClientOwnedTruth) {
  FakeOpenPort port;
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

}  // namespace
}  // namespace manyforge::core
