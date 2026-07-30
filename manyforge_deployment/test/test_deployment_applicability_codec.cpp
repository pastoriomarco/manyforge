#include <gtest/gtest.h>

#include <algorithm>
#include <manyforge_core/types/deployment_applicability.hpp>
#include <manyforge_deployment/artifact_codec.hpp>
#include <manyforge_deployment/canonical_value.hpp>
#include <manyforge_deployment/content_identity.hpp>
#include <manyforge_deployment/deployment_applicability_codec.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <variant>

#include "proof_root_transfer_test.hpp"

namespace manyforge::deployment {
namespace {

constexpr std::string_view kZeroDigest =
    "sha256:0000000000000000000000000000000000000000000000000000000000000000";
constexpr std::string_view kOneDigest =
    "sha256:0000000000000000000000000000000000000000000000000000000000000001";

static_assert(
    !std::is_same_v<core::RuntimeUnitId, core::ExternalResourceAlias>);
static_assert(
    !std::is_same_v<core::ExternalResourceAlias, core::SiteOverlayValueRef>);
static_assert(
    !std::is_aggregate_v<TypedDeploymentApplicabilityInputsCandidate>);
static_assert(
    !std::is_default_constructible_v<
        TypedDeploymentApplicabilityInputsCandidate>);
static_assert(
    !std::is_aggregate_v<RevisionVerifiedDeploymentApplicabilityInputs>);
static_assert(
    !std::is_default_constructible_v<
        RevisionVerifiedDeploymentApplicabilityInputs>);

[[nodiscard]] std::string minimal_document(
    std::string desired = "[]", std::string providers = "[]",
    std::string external = "[]", bool include_external = true,
    std::optional<std::string> site_overlay_ref = std::nullopt) {
  std::string document =
      "{\"api_version\":\"manyforge.deployment.v1\","
      "\"desired_control_configurations\":" +
      std::move(desired) + ",\"capability_provider_bindings\":" +
      std::move(providers);
  if (include_external) {
    document += ",\"external_resources\":" + std::move(external);
  }
  if (site_overlay_ref.has_value()) {
    document += ",\"site_overlay_ref\":" + std::move(*site_overlay_ref);
  }
  document += "}";
  return document;
}

[[nodiscard]] std::string site_overlay_ref(
    std::string_view id = "plant_a",
    std::string_view revision = kZeroDigest,
    std::string_view non_secret_digest = kOneDigest) {
  return "{\"site_overlay_id\":\"" + std::string(id) +
         "\",\"site_overlay_revision\":\"" + std::string(revision) +
         "\",\"non_secret_digest\":\"" +
         std::string(non_secret_digest) + "\"}";
}

[[nodiscard]] std::string desired_route(
    std::string_view binding_id = "arm_binding",
    std::string_view binding_revision = kZeroDigest,
    std::string_view route_id = "standard_route") {
  return "{\"control_binding_id\":\"" + std::string(binding_id) +
         "\",\"control_binding_revision\":\"" +
         std::string(binding_revision) + "\",\"control_route_id\":\"" +
         std::string(route_id) + "\"}";
}

[[nodiscard]] std::string desired_configuration(
    std::string_view domain_id = "arm_domain",
    std::string_view domain_revision = kZeroDigest,
    std::string routes = desired_route()) {
  return "{\"control_domain_id\":\"" + std::string(domain_id) +
         "\",\"control_domain_revision\":\"" +
         std::string(domain_revision) +
         "\",\"controller_topology_id\":\"standard_topology\","
         "\"topology_revision\":\"" +
         std::string(kZeroDigest) +
         "\",\"activation_set_id\":\"trajectory\","
         "\"binding_routes\":[" +
         std::move(routes) + "]}";
}

[[nodiscard]] std::string endpoint_binding(
    std::string_view endpoint_role = "motion_plan",
    std::string_view source_kind = "runtime_unit",
    std::string_view source_id = "planner_runtime",
    std::string_view source_endpoint_role = "motion_plan_service") {
  return "{\"endpoint_role_id\":\"" + std::string(endpoint_role) +
         "\",\"source_kind\":\"" + std::string(source_kind) +
         "\",\"source_id\":\"" + std::string(source_id) +
         "\",\"source_endpoint_role_id\":\"" +
         std::string(source_endpoint_role) + "\"}";
}

[[nodiscard]] std::string target_binding_target() {
  return "{\"controllable_component_id\":\"arm\","
         "\"component_type_revision\":\"" +
         std::string(kZeroDigest) +
         "\",\"command_target_id\":\"arm_target\","
         "\"control_binding_id\":\"arm_binding\","
         "\"control_binding_revision\":\"" +
         std::string(kZeroDigest) + "\"}";
}

[[nodiscard]] std::string provider_binding(
    std::string_view binding_id = "planner_binding",
    std::string_view binding_revision = kZeroDigest,
    std::string_view scope = "target_binding",
    std::string target = target_binding_target(),
    std::string endpoints = endpoint_binding()) {
  return "{\"capability_provider_binding_id\":\"" +
         std::string(binding_id) +
         "\",\"capability_provider_binding_revision\":\"" +
         std::string(binding_revision) +
         "\",\"capability_id\":\"planning.motion.pose_goal\","
         "\"capability_definition_revision\":\"" +
         std::string(kZeroDigest) + "\",\"scope\":\"" +
         std::string(scope) + "\",\"target\":" + std::move(target) +
         ",\"provider_instance_id\":\"planner_provider\","
         "\"provider_manifest_id\":\"planner_manifest\","
         "\"provider_manifest_revision\":\"" +
         std::string(kZeroDigest) + "\",\"endpoint_bindings\":[" +
         std::move(endpoints) + "]}";
}

[[nodiscard]] std::string external_resource(
    std::string_view alias = "planner_endpoint",
    std::string_view kind = "service_endpoint",
    std::string_view overlay_ref = "resources.planner.endpoint") {
  return "{\"external_resource_alias\":\"" + std::string(alias) +
         "\",\"resource_kind\":\"" + std::string(kind) +
         "\",\"provider_instance_id\":\"planner_provider\","
         "\"provider_manifest_id\":\"planner_manifest\","
         "\"provider_manifest_revision\":\"" +
         std::string(kZeroDigest) +
         "\",\"endpoint_role_id\":\"motion_plan_service\","
         "\"site_overlay_value_ref\":\"" + std::string(overlay_ref) +
         "\"}";
}

[[nodiscard]] TypedDeploymentApplicabilityInputsCandidate decode(
    std::string_view document) {
  return decode_deployment_applicability_inputs_candidate(
      parse_strict_json_model_yaml(document));
}

void expect_decode_error_containing(std::string document,
                                    std::string_view expected_fragment) {
  try {
    (void)decode(document);
    FAIL() << "expected DeploymentApplicabilityCodecError";
  } catch (const DeploymentApplicabilityCodecError& error) {
    EXPECT_NE(std::string_view(error.what()).find(expected_fragment),
              std::string_view::npos)
        << error.what();
  }
}

[[nodiscard]] std::string document_from_verified(
    const RevisionVerifiedDeploymentApplicabilityInputs& verified) {
  return minimal_document(
      canonical_json(
          verified.normalized_desired_control_configurations()),
      canonical_json(verified.normalized_capability_provider_bindings()),
      canonical_json(verified.normalized_external_resources()),
      verified.external_resources_was_present(),
      verified.normalized_site_overlay_ref().has_value()
          ? std::optional<std::string>(canonical_json(
                *verified.normalized_site_overlay_ref()))
          : std::nullopt);
}

[[nodiscard]] const CanonicalValue& first_array_item(
    const CanonicalValue& value) {
  const auto& array = std::get<CanonicalValue::Array>(value.storage());
  EXPECT_FALSE(array.empty());
  return array.front();
}

[[nodiscard]] std::string independent_provider_binding_identity(
    const CanonicalValue& binding) {
  CanonicalValue::Object payload;
  bool own_field_found = false;
  for (const auto& [name, value] : binding.as_object()) {
    if (name == "capability_provider_binding_revision") {
      own_field_found = true;
      continue;
    }
    payload.emplace_back(name, value);
  }
  EXPECT_TRUE(own_field_found);

  std::string preimage{"manyforge.content-identity.v1"};
  preimage.push_back('\0');
  preimage.append("capability-provider-binding-revision");
  preimage.push_back('\0');
  preimage.append("manyforge.deployment.v1");
  preimage.push_back('\0');
  preimage.append(canonical_json(CanonicalValue(std::move(payload))));
  return sha256_digest(preimage);
}

[[nodiscard]] std::string replace_first(std::string source,
                                        std::string_view from,
                                        std::string_view to) {
  const std::size_t position = source.find(from);
  EXPECT_NE(position, std::string::npos);
  if (position != std::string::npos) {
    source.replace(position, from.size(), to);
  }
  return source;
}

template <typename RowFactory>
[[nodiscard]] std::string comma_joined_rows(std::size_t count,
                                            RowFactory make_row) {
  std::string rows;
  for (std::size_t index = 0; index < count; ++index) {
    if (!rows.empty()) {
      rows.push_back(',');
    }
    rows += make_row(index);
  }
  return rows;
}

TEST(DeploymentApplicabilityCodec,
     DecodesClosedVariantsAndNormalizesEveryOwnedCollection) {
  const std::string routes =
      desired_route("z_binding", kZeroDigest, "z_route") + "," +
      desired_route("a_binding", kOneDigest, "a_route");
  const std::string desired =
      "[" + desired_configuration("z_domain", kZeroDigest, routes) + "," +
      desired_configuration("a_domain", kOneDigest, "") + "]";

  const std::string endpoints =
      endpoint_binding("z_role", "external_resource", "planner_endpoint",
                       "z_source_role") +
      "," +
      endpoint_binding("a_role", "runtime_unit", "planner_runtime",
                       "a_source_role");
  const std::string providers =
      "[" + provider_binding("z_provider_binding", kZeroDigest,
                             "target_binding", target_binding_target(),
                             endpoints) +
      "," +
      provider_binding("a_provider_binding", kOneDigest, "cell_global",
                       "{\"cell_id\":\"cell_a\"}", "") +
      "]";
  const std::string external =
      "[" + external_resource("z_endpoint") + "," +
      external_resource("a_endpoint", "model_endpoint",
                        "resources.site_a.endpoint") +
      "]";

  const auto candidate =
      decode(minimal_document(desired, providers, external));
  const auto& value = candidate.value();
  ASSERT_EQ(value.desired_control_configurations.size(), 2U);
  EXPECT_EQ(value.desired_control_configurations[0].control_domain_id.value(),
            "a_domain");
  ASSERT_EQ(value.desired_control_configurations[1].binding_routes.size(),
            2U);
  EXPECT_EQ(value.desired_control_configurations[1]
                .binding_routes[0]
                .control_binding_id.value(),
            "a_binding");

  ASSERT_EQ(value.capability_provider_bindings.size(), 2U);
  EXPECT_EQ(value.capability_provider_bindings[0]
                .capability_provider_binding_id.value(),
            "a_provider_binding");
  const auto& target_binding = value.capability_provider_bindings[1];
  ASSERT_EQ(target_binding.endpoint_bindings.size(), 2U);
  EXPECT_EQ(target_binding.endpoint_bindings[0].endpoint_role_id.value(),
            "a_role");
  EXPECT_TRUE(std::holds_alternative<core::RuntimeUnitId>(
      target_binding.endpoint_bindings[0].source_id));
  EXPECT_TRUE(std::holds_alternative<core::ExternalResourceAlias>(
      target_binding.endpoint_bindings[1].source_id));
  EXPECT_TRUE(
      std::holds_alternative<
          core::TargetBindingCapabilityProviderTargetDTO>(
          target_binding.target));

  ASSERT_EQ(value.external_resources.size(), 2U);
  EXPECT_EQ(value.external_resources[0].external_resource_alias.value(),
            "a_endpoint");
  EXPECT_EQ(value.external_resources[0].site_overlay_value_ref.value(),
            "resources.site_a.endpoint");
  EXPECT_TRUE(candidate.external_resources_was_present());

  const std::string canonical_desired =
      canonical_json(candidate.normalized_desired_control_configurations());
  EXPECT_LT(canonical_desired.find("\"a_domain\""),
            canonical_desired.find("\"z_domain\""));
  const std::string canonical_providers =
      canonical_json(candidate.normalized_capability_provider_bindings());
  EXPECT_LT(canonical_providers.find("\"a_provider_binding\""),
            canonical_providers.find("\"z_provider_binding\""));
}

TEST(DeploymentApplicabilityCodec,
     RvalueTransfersPreserveTypedAndRevisionVerifiedInputsForReuse) {
  const auto typed = decode(minimal_document(
      "[" + desired_configuration() + "]",
      "[" + provider_binding() + "]",
      "[" + external_resource() + "]", true, site_overlay_ref()));
  const auto projection = [](const auto& value) {
    return std::tuple{
        value.value(),
        value.normalized_desired_control_configurations(),
        value.normalized_capability_provider_bindings(),
        value.normalized_external_resources(),
        value.site_overlay_ref_presence(),
        value.normalized_site_overlay_ref(),
        value.external_resources_was_present()};
  };
  test::expect_source_preserving_rvalue_transfers(typed, projection);
  test::expect_immutable_shared_copy_storage(
      typed,
      [](const TypedDeploymentApplicabilityInputsCandidate& value)
          -> const core::DeploymentApplicabilityInputsDTO& {
        return value.value();
      });

  const auto typed_destination = std::move(typed);
  const auto first_verified =
      compute_capability_provider_binding_revisions(typed);
  const auto second_verified =
      compute_capability_provider_binding_revisions(typed_destination);
  EXPECT_EQ(first_verified.value(), second_verified.value());
  EXPECT_EQ(projection(first_verified), projection(second_verified));

  test::expect_source_preserving_rvalue_transfers(first_verified,
                                                   projection);
  test::expect_immutable_shared_copy_storage(
      first_verified,
      [](const RevisionVerifiedDeploymentApplicabilityInputs& value)
          -> const core::DeploymentApplicabilityInputsDTO& {
        return value.value();
      });

  const auto consumer_candidate =
      decode(document_from_verified(first_verified));
  const auto consumer_verified =
      verify_capability_provider_binding_revisions(consumer_candidate);
  EXPECT_EQ(&consumer_verified.value(), &consumer_candidate.value());
  EXPECT_EQ(&consumer_verified.normalized_capability_provider_bindings(),
            &consumer_candidate.normalized_capability_provider_bindings());
}

TEST(DeploymentApplicabilityCodec,
     SupportsAllThreeScopeTargetsAndBothEndpointSourceAlternatives) {
  const std::string providers =
      "[" +
      provider_binding("cell_provider", kZeroDigest, "cell_global",
                       "{\"cell_id\":\"cell_a\"}", "") +
      "," +
      provider_binding(
          "domain_provider", kZeroDigest, "control_domain",
          "{\"control_domain_id\":\"arm_domain\","
          "\"control_domain_revision\":\"" +
              std::string(kZeroDigest) +
              "\",\"controller_topology_id\":\"standard_topology\","
              "\"topology_revision\":\"" +
              std::string(kZeroDigest) + "\"}",
          endpoint_binding("domain_status", "runtime_unit",
                           "controller_runtime", "controller_status")) +
      "," +
      provider_binding(
          "target_provider", kZeroDigest, "target_binding",
          target_binding_target(),
          endpoint_binding("planning", "external_resource",
                           "planner_endpoint", "planning_service")) +
      "]";

  const auto candidate =
      decode(minimal_document("[]", providers,
                              "[" + external_resource() + "]"));
  ASSERT_EQ(candidate.value().capability_provider_bindings.size(), 3U);
  EXPECT_TRUE(
      std::holds_alternative<core::CellGlobalCapabilityProviderTargetDTO>(
          candidate.value().capability_provider_bindings[0].target));
  EXPECT_TRUE(
      std::holds_alternative<
          core::ControlDomainCapabilityProviderTargetDTO>(
          candidate.value().capability_provider_bindings[1].target));
  EXPECT_TRUE(
      std::holds_alternative<
          core::TargetBindingCapabilityProviderTargetDTO>(
          candidate.value().capability_provider_bindings[2].target));
}

TEST(DeploymentApplicabilityCodec,
     ProducerAndConsumerUseExactNestedSemanticIdentity) {
  const auto candidate =
      decode(minimal_document("[]", "[" + provider_binding() + "]"));
  const auto produced =
      compute_capability_provider_binding_revisions(candidate);
  ASSERT_EQ(produced.value().capability_provider_bindings.size(), 1U);

  const CanonicalValue& normalized_binding =
      first_array_item(produced.normalized_capability_provider_bindings());
  const std::string independent =
      independent_provider_binding_identity(normalized_binding);
  EXPECT_EQ(produced.value()
                .capability_provider_bindings[0]
                .capability_provider_binding_revision.value(),
            independent);

  const std::string verified_document = document_from_verified(produced);
  const auto consumer_candidate = decode(verified_document);
  EXPECT_NO_THROW(
      (void)verify_capability_provider_binding_revisions(consumer_candidate));

  const std::string stale_document =
      replace_first(verified_document, "motion_plan_service",
                    "different_source_role");
  const auto stale_candidate = decode(stale_document);
  EXPECT_THROW(
      (void)verify_capability_provider_binding_revisions(stale_candidate),
      DeploymentApplicabilityCodecError);
  const auto reminted =
      compute_capability_provider_binding_revisions(stale_candidate);
  EXPECT_NE(reminted.value()
                .capability_provider_bindings[0]
                .capability_provider_binding_revision,
            produced.value()
                .capability_provider_bindings[0]
                .capability_provider_binding_revision);
}

TEST(DeploymentApplicabilityCodec,
     EquivalentSourceOrderHasOneNormalizedIdentity) {
  const std::string forward_endpoints =
      endpoint_binding("a", "runtime_unit", "runtime_a", "source_a") + "," +
      endpoint_binding("z", "external_resource", "external_z", "source_z");
  const std::string reverse_endpoints =
      endpoint_binding("z", "external_resource", "external_z", "source_z") +
      "," + endpoint_binding("a", "runtime_unit", "runtime_a", "source_a");

  const auto forward = compute_capability_provider_binding_revisions(
      decode(minimal_document(
          "[]",
          "[" + provider_binding("provider", kZeroDigest, "target_binding",
                                 target_binding_target(),
                                 forward_endpoints) +
              "]")));
  const auto reverse = compute_capability_provider_binding_revisions(
      decode(minimal_document(
          "[]",
          "[" + provider_binding("provider", kOneDigest, "target_binding",
                                 target_binding_target(),
                                 reverse_endpoints) +
              "]")));

  EXPECT_EQ(forward.value(), reverse.value());
  EXPECT_EQ(canonical_json(forward.normalized_capability_provider_bindings()),
            canonical_json(reverse.normalized_capability_provider_bindings()));
}

TEST(DeploymentApplicabilityCodec,
     OptionalExternalTablePreservesSourcePresence) {
  const auto absent =
      decode(minimal_document("[]", "[]", "[]", false));
  EXPECT_FALSE(absent.external_resources_was_present());
  EXPECT_TRUE(absent.value().external_resources.empty());
  EXPECT_EQ(canonical_json(absent.normalized_external_resources()), "[]");

  const auto explicit_empty = decode(minimal_document());
  EXPECT_TRUE(explicit_empty.external_resources_was_present());
  EXPECT_TRUE(explicit_empty.value().external_resources.empty());
  EXPECT_EQ(absent.value(), explicit_empty.value());
}

TEST(DeploymentApplicabilityCodec,
     SiteOverlayRefPreservesAbsentNullAndExactValueSourceStates) {
  const auto absent = decode(minimal_document());
  EXPECT_EQ(absent.site_overlay_ref_presence(),
            SiteOverlayRefSourcePresence::kAbsent);
  EXPECT_FALSE(absent.normalized_site_overlay_ref().has_value());
  EXPECT_FALSE(absent.value().site_overlay_ref.has_value());

  const auto explicit_null =
      decode(minimal_document("[]", "[]", "[]", true, "null"));
  EXPECT_EQ(explicit_null.site_overlay_ref_presence(),
            SiteOverlayRefSourcePresence::kExplicitNull);
  ASSERT_TRUE(explicit_null.normalized_site_overlay_ref().has_value());
  EXPECT_TRUE(std::holds_alternative<std::nullptr_t>(
      explicit_null.normalized_site_overlay_ref()->storage()));
  EXPECT_FALSE(explicit_null.value().site_overlay_ref.has_value());
  EXPECT_EQ(absent.value(), explicit_null.value());

  const auto exact = decode(minimal_document(
      "[]", "[]", "[]", true, site_overlay_ref()));
  EXPECT_EQ(exact.site_overlay_ref_presence(),
            SiteOverlayRefSourcePresence::kValue);
  ASSERT_TRUE(exact.value().site_overlay_ref.has_value());
  EXPECT_EQ(exact.value().site_overlay_ref->site_overlay_id.value(),
            "plant_a");
  EXPECT_EQ(exact.value().site_overlay_ref->site_overlay_revision.value(),
            kZeroDigest);
  EXPECT_EQ(exact.value().site_overlay_ref->non_secret_digest.value(),
            kOneDigest);
  ASSERT_TRUE(exact.normalized_site_overlay_ref().has_value());
  EXPECT_EQ(canonical_json(*exact.normalized_site_overlay_ref()),
            "{\"non_secret_digest\":\"" + std::string(kOneDigest) +
                "\",\"site_overlay_id\":\"plant_a\","
                "\"site_overlay_revision\":\"" +
                std::string(kZeroDigest) + "\"}");

  const auto verified =
      verify_capability_provider_binding_revisions(exact);
  EXPECT_EQ(verified.site_overlay_ref_presence(),
            SiteOverlayRefSourcePresence::kValue);
  EXPECT_EQ(verified.normalized_site_overlay_ref(),
            exact.normalized_site_overlay_ref());
}

TEST(DeploymentApplicabilityCodec,
     FocusedProjectionRequiresItsVersionAndTwoRequiredTables) {
  EXPECT_THROW(
      (void)decode(
          "{\"desired_control_configurations\":[],"
          "\"capability_provider_bindings\":[]}"),
      DeploymentApplicabilityCodecError);
  EXPECT_THROW(
      (void)decode(
          "{\"api_version\":\"manyforge.deployment.v0\","
          "\"desired_control_configurations\":[],"
          "\"capability_provider_bindings\":[]}"),
      DeploymentApplicabilityCodecError);
  EXPECT_THROW(
      (void)decode(
          "{\"api_version\":\"manyforge.deployment.v1\","
          "\"capability_provider_bindings\":[]}"),
      DeploymentApplicabilityCodecError);
  EXPECT_THROW(
      (void)decode(
          "{\"api_version\":\"manyforge.deployment.v1\","
          "\"desired_control_configurations\":[]}"),
      DeploymentApplicabilityCodecError);
}

TEST(DeploymentApplicabilityCodec,
     ExactShapesAndClosedEnumsRejectMalformedRows) {
  const std::string valid_provider = provider_binding();
  EXPECT_THROW(
      (void)decode(minimal_document(
          "[" + replace_first(desired_configuration(), "\"binding_routes\"",
                              "\"unknown_routes\"") +
              "]",
          "[]")),
      DeploymentApplicabilityCodecError);
  EXPECT_THROW(
      (void)decode(minimal_document(
          "[]",
          "[" +
              replace_first(valid_provider, "\"target_binding\"",
                            "\"unknown_scope\"") +
              "]")),
      DeploymentApplicabilityCodecError);
  EXPECT_THROW(
      (void)decode(minimal_document(
          "[]",
          "[" +
              replace_first(valid_provider, "\"target\":{",
                            "\"target\":{\"cell_id\":\"cell_a\","
                            "\"unexpected\":\"x\",") +
              "]")),
      DeploymentApplicabilityCodecError);
  EXPECT_THROW(
      (void)decode(minimal_document(
          "[]",
          "[" +
              replace_first(valid_provider, "\"runtime_unit\"",
                            "\"unknown_source\"") +
              "]")),
      DeploymentApplicabilityCodecError);
  EXPECT_THROW(
      (void)decode(minimal_document(
          "[]",
          "[" +
              replace_first(valid_provider, "\"planner_runtime\"",
                            "\"bad/runtime\"") +
              "]")),
      DeploymentApplicabilityCodecError);
  EXPECT_THROW(
      (void)decode(minimal_document(
          "[]", "[]",
          "[" + external_resource("alias", "unknown_kind") + "]")),
      DeploymentApplicabilityCodecError);
  EXPECT_THROW(
      (void)decode(minimal_document(
          "[]", "[]", "[" + external_resource("alias", "service_endpoint", "") +
                          "]")),
      DeploymentApplicabilityCodecError);
  EXPECT_THROW((void)decode(minimal_document("[]", "[]", "null")),
               DeploymentApplicabilityCodecError);
  EXPECT_THROW(
      (void)decode(minimal_document(
          "[]", "[]", "[]", true,
          "{\"site_overlay_id\":\"plant_a\","
          "\"site_overlay_revision\":\"" +
              std::string(kZeroDigest) + "\"}")),
      DeploymentApplicabilityCodecError);
  EXPECT_THROW(
      (void)decode(minimal_document(
          "[]", "[]", "[]", true,
          site_overlay_ref("bad/site"))),
      DeploymentApplicabilityCodecError);
  EXPECT_THROW(
      (void)decode(minimal_document(
          "[]", "[]", "[]", true,
          site_overlay_ref("plant_a", kZeroDigest, "not-a-digest"))),
      DeploymentApplicabilityCodecError);

  const std::string uppercase_digest =
      "sha256:AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA";
  EXPECT_THROW(
      (void)decode(minimal_document(
          "[" + desired_configuration("arm_domain", uppercase_digest) + "]",
          "[]")),
      DeploymentApplicabilityCodecError);
}

TEST(DeploymentApplicabilityCodec,
     DuplicateKeysRejectAtEveryOwnedCollectionBoundary) {
  const std::string route = desired_route();
  EXPECT_THROW(
      (void)decode(minimal_document(
          "[" + desired_configuration("domain", kZeroDigest,
                                       route + "," + route) +
              "]",
          "[]")),
      DeploymentApplicabilityCodecError);

  const std::string endpoint = endpoint_binding();
  EXPECT_THROW(
      (void)decode(minimal_document(
          "[]",
          "[" + provider_binding("provider", kZeroDigest, "target_binding",
                                 target_binding_target(),
                                 endpoint + "," + endpoint) +
              "]")),
      DeploymentApplicabilityCodecError);

  const std::string desired = desired_configuration();
  EXPECT_THROW(
      (void)decode(minimal_document("[" + desired + "," + desired + "]",
                                    "[]")),
      DeploymentApplicabilityCodecError);

  const std::string provider = provider_binding();
  EXPECT_THROW(
      (void)decode(minimal_document(
          "[]", "[" + provider + "," + provider + "]")),
      DeploymentApplicabilityCodecError);

  const std::string external = external_resource();
  EXPECT_THROW(
      (void)decode(minimal_document(
          "[]", "[]", "[" + external + "," + external + "]")),
      DeploymentApplicabilityCodecError);
}

TEST(DeploymentApplicabilityCodec,
     EndpointRoleCeilingAccepts256AndRejects257BeforeTypedAllocation) {
  std::string first_256;
  for (std::size_t index = 0; index < 257U; ++index) {
    if (index < 256U) {
      if (!first_256.empty()) {
        first_256.push_back(',');
      }
      first_256 += endpoint_binding(
          "role_" + std::to_string(index), "runtime_unit", "runtime",
          "source_" + std::to_string(index));
    }
  }
  const std::string endpoints =
      first_256 + "," +
      endpoint_binding("role_256", "runtime_unit", "runtime", "source_256");
  EXPECT_NO_THROW((void)decode(minimal_document(
      "[]",
      "[" + provider_binding("provider", kZeroDigest, "target_binding",
                             target_binding_target(), first_256) +
          "]")));
  EXPECT_THROW(
      (void)decode(minimal_document(
          "[]",
          "[" + provider_binding("provider", kZeroDigest, "target_binding",
                                 target_binding_target(), endpoints) +
              "]")),
      DeploymentApplicabilityCodecError);
}

TEST(DeploymentApplicabilityCodec,
     TupleKeysPermitStaleSameIdRowsForLaterApplicabilityRejection) {
  // Specs currently define these substrate collections by complete typed
  // identity. Whether two revisions under one stable ID are stale/extra
  // occurrence inputs is deliberately resolved against the graph/provider
  // closure in the later C0.2n qualifier.
  const auto candidate = decode(minimal_document(
      "[" + desired_configuration("domain", kZeroDigest) + "," +
          desired_configuration("domain", kOneDigest) + "]",
      "[" + provider_binding("provider", kZeroDigest) + "," +
          provider_binding("provider", kOneDigest) + "]"));
  EXPECT_EQ(candidate.value().desired_control_configurations.size(), 2U);
  EXPECT_EQ(candidate.value().capability_provider_bindings.size(), 2U);
}

TEST(DeploymentApplicabilityCodec,
     ProducerRejectsRowsThatCollapseToOneSemanticIdentity) {
  const auto candidate = decode(minimal_document(
      "[]", "[" + provider_binding("provider", kZeroDigest) + "," +
                provider_binding("provider", kOneDigest) + "]"));
  EXPECT_THROW(
      (void)compute_capability_provider_binding_revisions(candidate),
      DeploymentApplicabilityCodecError);
}

TEST(DeploymentApplicabilityCodec,
     SiteOverlayValueRefsUseTheExactPortableV1Grammar) {
  const std::string maximum_ref =
      "a" + std::string(127U, 'z');
  const auto candidate = decode(minimal_document(
      "[]", "[]",
      "[" + external_resource("endpoint", "service_endpoint", maximum_ref) +
          "]"));
  ASSERT_EQ(candidate.value().external_resources.size(), 1U);
  EXPECT_EQ(candidate.value()
                .external_resources[0]
                .site_overlay_value_ref.value(),
            maximum_ref);

  for (const std::string_view invalid :
       {"", ".leading", "resources/site", "resources site",
        "resources[0]", "resources:site"}) {
    EXPECT_THROW(
        (void)decode(minimal_document(
            "[]", "[]",
            "[" + external_resource("endpoint", "service_endpoint",
                                    invalid) +
                "]")),
        DeploymentApplicabilityCodecError)
        << invalid;
  }
  EXPECT_THROW(
      (void)decode(minimal_document(
          "[]", "[]",
          "[" +
              external_resource("endpoint", "service_endpoint",
                                "a" + std::string(128U, 'z')) +
              "]")),
      DeploymentApplicabilityCodecError);
}

TEST(DeploymentApplicabilityCodec,
     ExternalResourceAliasesUseTheExactPortableV1Grammar) {
  const auto table_for_alias = [](std::string_view alias) {
    std::string table{"["};
    table += external_resource(alias);
    table.push_back(']');
    return table;
  };

  const std::string maximum_alias = "a" + std::string(127U, 'z');
  const auto candidate =
      decode(minimal_document("[]", "[]", table_for_alias(maximum_alias)));
  ASSERT_EQ(candidate.value().external_resources.size(), 1U);
  EXPECT_EQ(candidate.value()
                .external_resources[0]
                .external_resource_alias.value(),
            maximum_alias);

  constexpr std::string_view kAliasPath =
      "$.external_resources[0].external_resource_alias: expected a 1..128 "
      "byte portable token";
  for (const std::string_view invalid :
       {"", ".leading", "_leading", "-leading", "resource/endpoint",
        "resource endpoint", "resource[0]", "resource:endpoint",
        R"(\u00e9ndpoint)"}) {
    SCOPED_TRACE(invalid);
    expect_decode_error_containing(
        minimal_document("[]", "[]", table_for_alias(invalid)),
        kAliasPath);
  }

  expect_decode_error_containing(
      minimal_document("[]", "[]",
                       table_for_alias("a" + std::string(128U, 'z'))),
      kAliasPath);
}

TEST(DeploymentApplicabilityCodec,
     ExternalResourceCeilingAccepts4096AndRejects4097) {
  std::string first_4096;
  for (std::size_t index = 0;
       index < core::kMaxDeploymentApplicabilityRows + 1U; ++index) {
    if (index < core::kMaxDeploymentApplicabilityRows) {
      if (!first_4096.empty()) {
        first_4096.push_back(',');
      }
      first_4096 += external_resource(
          "resource_" + std::to_string(index), "service_endpoint",
          "resources.endpoint_" + std::to_string(index));
    }
  }
  EXPECT_NO_THROW((void)decode(
      minimal_document("[]", "[]", "[" + first_4096 + "]")));
  EXPECT_THROW(
      (void)decode(minimal_document(
          "[]", "[]",
          "[" + first_4096 + "," +
              external_resource("resource_4096", "service_endpoint",
                                "resources.endpoint_4096") +
              "]")),
      DeploymentApplicabilityCodecError);
}

TEST(DeploymentApplicabilityCodec,
     DesiredConfigurationCeilingAccepts4096AndRejects4097) {
  const std::string boundary_rows = comma_joined_rows(
      core::kMaxDeploymentApplicabilityRows, [](std::size_t index) {
        return desired_configuration(
            "domain_" + std::to_string(index), kZeroDigest, "");
      });

  const auto accepted =
      decode(minimal_document("[" + boundary_rows + "]", "[]"));
  EXPECT_EQ(accepted.value().desired_control_configurations.size(),
            core::kMaxDeploymentApplicabilityRows);

  expect_decode_error_containing(
      minimal_document(
          "[" + boundary_rows + "," +
              desired_configuration("domain_4096", kZeroDigest, "") + "]",
          "[]"),
      "$.desired_control_configurations: configuration count exceeds");
}

TEST(DeploymentApplicabilityCodec,
     CapabilityProviderBindingCeilingAccepts4096AndRejects4097) {
  const std::string boundary_rows = comma_joined_rows(
      core::kMaxDeploymentApplicabilityRows, [](std::size_t index) {
        return provider_binding("provider_" + std::to_string(index));
      });

  const auto accepted =
      decode(minimal_document("[]", "[" + boundary_rows + "]"));
  EXPECT_EQ(accepted.value().capability_provider_bindings.size(),
            core::kMaxDeploymentApplicabilityRows);

  expect_decode_error_containing(
      minimal_document(
          "[]",
          "[" + boundary_rows + "," + provider_binding("provider_4096") +
              "]"),
      "$.capability_provider_bindings: provider-binding count exceeds");
}

TEST(DeploymentApplicabilityCodec,
     DesiredBindingRouteCeilingAccepts4096AndRejects4097) {
  const std::string boundary_rows = comma_joined_rows(
      core::kMaxDeploymentApplicabilityRows, [](std::size_t index) {
        return desired_route("binding_" + std::to_string(index),
                             kZeroDigest,
                             "route_" + std::to_string(index));
      });

  const auto accepted = decode(minimal_document(
      "[" +
          desired_configuration("domain", kZeroDigest, boundary_rows) +
          "]",
      "[]"));
  ASSERT_EQ(accepted.value().desired_control_configurations.size(), 1U);
  EXPECT_EQ(
      accepted.value().desired_control_configurations[0].binding_routes.size(),
      core::kMaxDeploymentApplicabilityRows);

  expect_decode_error_containing(
      minimal_document(
          "[" +
              desired_configuration(
                  "domain", kZeroDigest,
                  boundary_rows + "," +
                      desired_route("binding_4096", kZeroDigest,
                                    "route_4096")) +
              "]",
          "[]"),
      "$.desired_control_configurations[0].binding_routes: binding-route "
      "count exceeds");
}

}  // namespace
}  // namespace manyforge::deployment
