#include <gtest/gtest.h>

#include <algorithm>
#include <manyforge_core/types/deployment_applicability.hpp>
#include <manyforge_deployment/artifact_codec.hpp>
#include <manyforge_deployment/canonical_value.hpp>
#include <manyforge_deployment/content_identity.hpp>
#include <manyforge_deployment/deployment_applicability_codec.hpp>
#include <manyforge_deployment/scene_pose_source_policy_codec.hpp>
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
static_assert(!std::is_default_constructible_v<
              TypedDeploymentApplicabilityInputsCandidate>);
static_assert(
    !std::is_aggregate_v<RevisionVerifiedDeploymentApplicabilityInputs>);
static_assert(!std::is_default_constructible_v<
              RevisionVerifiedDeploymentApplicabilityInputs>);

[[nodiscard]] std::string
minimal_document(std::string desired = "[]", std::string providers = "[]",
                 std::string external = "[]", bool include_external = true,
                 std::optional<std::string> site_overlay_ref = std::nullopt) {
  std::string document =
      "{\"api_version\":\"manyforge.deployment.v1\","
      "\"desired_control_configurations\":" +
      std::move(desired) +
      ",\"capability_provider_bindings\":" + std::move(providers);
  if (include_external) {
    document += ",\"external_resources\":" + std::move(external);
  }
  if (site_overlay_ref.has_value()) {
    document += ",\"site_overlay_ref\":" + std::move(*site_overlay_ref);
  }
  document += "}";
  return document;
}

[[nodiscard]] std::string with_policies(std::string document,
                                        std::string policies) {
  EXPECT_FALSE(document.empty());
  EXPECT_EQ(document.back(), '}');
  document.pop_back();
  document += ",\"policies\":" + std::move(policies) + "}";
  return document;
}

[[nodiscard]] std::string
interrupt_config(std::string_view queue_depth = "64",
                 std::string_view cancel_validation_timeout_ns = "1000000",
                 std::string_view motion_scope_arm_timeout_ns = "2000000",
                 std::string_view supervisory_poll_rate_hz = "10") {
  return "{\"queue_depth\":" + std::string(queue_depth) +
         ",\"cancel_validation_timeout_ns\":" +
         std::string(cancel_validation_timeout_ns) +
         ",\"motion_scope_arm_timeout_ns\":" +
         std::string(motion_scope_arm_timeout_ns) +
         ",\"supervisory_poll_rate_hz\":" +
         std::string(supervisory_poll_rate_hz) + "}";
}

[[nodiscard]] std::string interrupt_source(
    std::string_view source_id, std::string_view provider_binding_id,
    std::string_view source_class = "fast_execution_relevant",
    std::string_view kind = "level", std::string_view priority = "100",
    std::string_view optional_fields = "") {
  return "{\"source_id\":\"" + std::string(source_id) +
         "\",\"capability_provider_binding_id\":\"" +
         std::string(provider_binding_id) +
         "\",\"adapter_capability_id\":\"interrupt.signal\","
         "\"signal\":\"halt\",\"source_class\":\"" +
         std::string(source_class) + "\",\"kind\":\"" + std::string(kind) +
         "\",\"priority\":" + std::string(priority) +
         std::string(optional_fields) + "}";
}

[[nodiscard]] std::string
interrupt_policy(std::string_view policy_id, std::string_view source_id,
                 std::string_view affected_phases = "[\"executing\"]",
                 std::string_view action = "preempt_trajectory",
                 std::string_view disposition = "await_recovery_action",
                 std::string_view scope_kind = "static",
                 std::string_view control_domain_ids = "[\"arm_domain\"]",
                 std::string_view resource_ids = "[]",
                 std::string_view optional_fields = "") {
  return "{\"policy_id\":\"" + std::string(policy_id) + "\",\"source_id\":\"" +
         std::string(source_id) +
         "\",\"controllable_component_ids\":[],\"command_target_ids\":[],"
         "\"control_domain_ids\":" +
         std::string(control_domain_ids) +
         ",\"control_binding_ids\":[],\"resource_ids\":" +
         std::string(resource_ids) +
         ",\"affected_phases\":" + std::string(affected_phases) +
         ",\"preemption_action\":\"" + std::string(action) +
         "\",\"post_latch_disposition\":\"" + std::string(disposition) +
         "\",\"scope_kind\":\"" + std::string(scope_kind) + "\"" +
         std::string(optional_fields) + "}";
}

[[nodiscard]] std::string
interrupt_policy_envelope(std::string sources, std::string policies,
                          std::string config = interrupt_config(),
                          std::string retained_policy_records = "[]") {
  return "{\"retained_policy_records\":" + std::move(retained_policy_records) +
         ",\"stop_stage_parameters\":[],"
         "\"commissioned_stop_interfaces\":[],\"interrupt_sources\":[" +
         std::move(sources) + "],\"interrupt_policies\":[" +
         std::move(policies) + "],\"interrupt_config\":" + std::move(config) +
         "}";
}

[[nodiscard]] std::string
catalog_record(std::string_view kind, std::string_view id,
               std::string_view schema_id,
               std::string_view revision = kZeroDigest,
               std::string_view schema_revision = kOneDigest) {
  return "{\"record_kind\":\"" + std::string(kind) + "\",\"record_id\":\"" +
         std::string(id) + "\",\"record_revision\":\"" + std::string(revision) +
         "\",\"schema_id\":\"" + std::string(schema_id) +
         "\",\"schema_revision\":\"" + std::string(schema_revision) +
         "\",\"content_digest\":\"" + std::string(kZeroDigest) +
         "\",\"content_addressed_store_key\":\"sha256/" +
         std::string(64U, '0') + "\"}";
}

[[nodiscard]] std::string
site_overlay_ref(std::string_view id = "plant_a",
                 std::string_view revision = kZeroDigest,
                 std::string_view non_secret_digest = kOneDigest) {
  return "{\"site_overlay_id\":\"" + std::string(id) +
         "\",\"site_overlay_revision\":\"" + std::string(revision) +
         "\",\"non_secret_digest\":\"" + std::string(non_secret_digest) + "\"}";
}

[[nodiscard]] std::string
desired_route(std::string_view binding_id = "arm_binding",
              std::string_view binding_revision = kZeroDigest,
              std::string_view route_id = "standard_route") {
  return "{\"control_binding_id\":\"" + std::string(binding_id) +
         "\",\"control_binding_revision\":\"" + std::string(binding_revision) +
         "\",\"control_route_id\":\"" + std::string(route_id) + "\"}";
}

[[nodiscard]] std::string
desired_configuration(std::string_view domain_id = "arm_domain",
                      std::string_view domain_revision = kZeroDigest,
                      std::string routes = desired_route()) {
  return "{\"control_domain_id\":\"" + std::string(domain_id) +
         "\",\"control_domain_revision\":\"" + std::string(domain_revision) +
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

[[nodiscard]] std::string
provider_binding(std::string_view binding_id = "planner_binding",
                 std::string_view binding_revision = kZeroDigest,
                 std::string_view scope = "target_binding",
                 std::string target = target_binding_target(),
                 std::string endpoints = endpoint_binding()) {
  return "{\"capability_provider_binding_id\":\"" + std::string(binding_id) +
         "\",\"capability_provider_binding_revision\":\"" +
         std::string(binding_revision) +
         "\",\"capability_id\":\"planning.motion.pose_goal\","
         "\"capability_definition_revision\":\"" +
         std::string(kZeroDigest) + "\",\"scope\":\"" + std::string(scope) +
         "\",\"target\":" + std::move(target) +
         ",\"provider_instance_id\":\"planner_provider\","
         "\"provider_manifest_id\":\"planner_manifest\","
         "\"provider_manifest_revision\":\"" +
         std::string(kZeroDigest) + "\",\"endpoint_bindings\":[" +
         std::move(endpoints) + "]}";
}

[[nodiscard]] std::string
external_resource(std::string_view alias = "planner_endpoint",
                  std::string_view kind = "service_endpoint",
                  std::string_view overlay_ref = "resources.planner.endpoint") {
  return "{\"external_resource_alias\":\"" + std::string(alias) +
         "\",\"resource_kind\":\"" + std::string(kind) +
         "\",\"provider_instance_id\":\"planner_provider\","
         "\"provider_manifest_id\":\"planner_manifest\","
         "\"provider_manifest_revision\":\"" +
         std::string(kZeroDigest) +
         "\",\"endpoint_role_id\":\"motion_plan_service\","
         "\"site_overlay_value_ref\":\"" +
         std::string(overlay_ref) + "\"}";
}

[[nodiscard]] TypedDeploymentApplicabilityInputsCandidate
decode(std::string_view document) {
  return decode_deployment_applicability_inputs_candidate(
      parse_strict_json_model_yaml(document));
}

void expect_decode_error_containing(std::string document,
                                    std::string_view expected_fragment) {
  try {
    (void)decode(document);
    FAIL() << "expected DeploymentApplicabilityCodecError";
  } catch (const DeploymentApplicabilityCodecError &error) {
    EXPECT_NE(std::string_view(error.what()).find(expected_fragment),
              std::string_view::npos)
        << error.what();
  }
}

[[nodiscard]] std::string document_from_verified(
    const RevisionVerifiedDeploymentApplicabilityInputs &verified) {
  return minimal_document(
      canonical_json(verified.normalized_desired_control_configurations()),
      canonical_json(verified.normalized_capability_provider_bindings()),
      canonical_json(verified.normalized_external_resources()),
      verified.external_resources_was_present(),
      verified.normalized_site_overlay_ref().has_value()
          ? std::optional<std::string>(
                canonical_json(*verified.normalized_site_overlay_ref()))
          : std::nullopt);
}

[[nodiscard]] const CanonicalValue &
first_array_item(const CanonicalValue &value) {
  const auto &array = std::get<CanonicalValue::Array>(value.storage());
  EXPECT_FALSE(array.empty());
  return array.front();
}

[[nodiscard]] std::string
independent_provider_binding_identity(const CanonicalValue &binding) {
  CanonicalValue::Object payload;
  bool own_field_found = false;
  for (const auto &[name, value] : binding.as_object()) {
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

[[nodiscard]] std::string
replace_first(std::string source, std::string_view from, std::string_view to) {
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
      "[" +
      provider_binding("z_provider_binding", kZeroDigest, "target_binding",
                       target_binding_target(), endpoints) +
      "," +
      provider_binding("a_provider_binding", kOneDigest, "cell_global",
                       "{\"cell_id\":\"cell_a\"}", "") +
      "]";
  const std::string external = "[" + external_resource("z_endpoint") + "," +
                               external_resource("a_endpoint", "model_endpoint",
                                                 "resources.site_a.endpoint") +
                               "]";

  const auto candidate = decode(minimal_document(desired, providers, external));
  const auto &value = candidate.value();
  ASSERT_EQ(value.desired_control_configurations.size(), 2U);
  EXPECT_EQ(value.desired_control_configurations[0].control_domain_id.value(),
            "a_domain");
  ASSERT_EQ(value.desired_control_configurations[1].binding_routes.size(), 2U);
  EXPECT_EQ(value.desired_control_configurations[1]
                .binding_routes[0]
                .control_binding_id.value(),
            "a_binding");

  ASSERT_EQ(value.capability_provider_bindings.size(), 2U);
  EXPECT_EQ(value.capability_provider_bindings[0]
                .capability_provider_binding_id.value(),
            "a_provider_binding");
  const auto &target_binding = value.capability_provider_bindings[1];
  ASSERT_EQ(target_binding.endpoint_bindings.size(), 2U);
  EXPECT_EQ(target_binding.endpoint_bindings[0].endpoint_role_id.value(),
            "a_role");
  EXPECT_TRUE(std::holds_alternative<core::RuntimeUnitId>(
      target_binding.endpoint_bindings[0].source_id));
  EXPECT_TRUE(std::holds_alternative<core::ExternalResourceAlias>(
      target_binding.endpoint_bindings[1].source_id));
  EXPECT_TRUE(
      std::holds_alternative<core::TargetBindingCapabilityProviderTargetDTO>(
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
      "[" + desired_configuration() + "]", "[" + provider_binding() + "]",
      "[" + external_resource() + "]", true, site_overlay_ref()));
  const auto projection = [](const auto &value) {
    return std::tuple{value.value(),
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
      [](const TypedDeploymentApplicabilityInputsCandidate &value)
          -> const core::DeploymentApplicabilityInputsDTO & {
        return value.value();
      });

  const auto typed_destination = std::move(typed);
  const auto first_verified =
      compute_capability_provider_binding_revisions(typed);
  const auto second_verified =
      compute_capability_provider_binding_revisions(typed_destination);
  EXPECT_EQ(first_verified.value(), second_verified.value());
  EXPECT_EQ(projection(first_verified), projection(second_verified));

  test::expect_source_preserving_rvalue_transfers(first_verified, projection);
  test::expect_immutable_shared_copy_storage(
      first_verified,
      [](const RevisionVerifiedDeploymentApplicabilityInputs &value)
          -> const core::DeploymentApplicabilityInputsDTO & {
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
      provider_binding("target_provider", kZeroDigest, "target_binding",
                       target_binding_target(),
                       endpoint_binding("planning", "external_resource",
                                        "planner_endpoint",
                                        "planning_service")) +
      "]";

  const auto candidate = decode(
      minimal_document("[]", providers, "[" + external_resource() + "]"));
  ASSERT_EQ(candidate.value().capability_provider_bindings.size(), 3U);
  EXPECT_TRUE(
      std::holds_alternative<core::CellGlobalCapabilityProviderTargetDTO>(
          candidate.value().capability_provider_bindings[0].target));
  EXPECT_TRUE(
      std::holds_alternative<core::ControlDomainCapabilityProviderTargetDTO>(
          candidate.value().capability_provider_bindings[1].target));
  EXPECT_TRUE(
      std::holds_alternative<core::TargetBindingCapabilityProviderTargetDTO>(
          candidate.value().capability_provider_bindings[2].target));
}

TEST(DeploymentApplicabilityCodec,
     ProducerAndConsumerUseExactNestedSemanticIdentity) {
  const auto candidate =
      decode(minimal_document("[]", "[" + provider_binding() + "]"));
  const auto produced =
      compute_capability_provider_binding_revisions(candidate);
  ASSERT_EQ(produced.value().capability_provider_bindings.size(), 1U);

  const CanonicalValue &normalized_binding =
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

  const std::string stale_document = replace_first(
      verified_document, "motion_plan_service", "different_source_role");
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

  const auto expect_stale_binding = [&](std::string_view from,
                                        std::string_view to) {
    EXPECT_THROW(
        {
          const auto stale = decode(replace_first(verified_document, from, to));
          static_cast<void>(
              verify_capability_provider_binding_revisions(stale));
        },
        DeploymentApplicabilityCodecError);
  };
  // A manifest match cannot repair a changed provider, binding, target, or
  // scope: each is part of the exact applicability-owned binding identity.
  expect_stale_binding("\"planner_provider\"", "\"foreign_provider\"");
  expect_stale_binding("\"planner_binding\"", "\"foreign_binding\"");
  expect_stale_binding("\"arm_target\"", "\"foreign_target\"");
  const auto alternate_target =
      canonical_json(parse_strict_json_model_yaml(
                         "{\"control_domain_id\":\"arm_domain\","
                         "\"control_domain_revision\":\"" +
                         std::string(kZeroDigest) +
                         "\",\"controller_topology_id\":\"standard_topology\","
                         "\"topology_revision\":\"" +
                         std::string(kZeroDigest) + "\"}")
                         .root());
  const auto canonical_target = canonical_json(
      parse_strict_json_model_yaml(target_binding_target()).root());
  const auto alternate_scope_document = replace_first(
      replace_first(verified_document, "\"scope\":\"target_binding\"",
                    "\"scope\":\"control_domain\""),
      "\"target\":" + canonical_target, "\"target\":" + alternate_target);
  const auto alternate_scope = decode(alternate_scope_document);
  EXPECT_THROW(static_cast<void>(verify_capability_provider_binding_revisions(
                   alternate_scope)),
               DeploymentApplicabilityCodecError);
}

TEST(DeploymentApplicabilityCodec,
     EquivalentSourceOrderHasOneNormalizedIdentity) {
  const std::string forward_endpoints =
      endpoint_binding("a", "runtime_unit", "runtime_a", "source_a") + "," +
      endpoint_binding("z", "external_resource", "external_z", "source_z");
  const std::string reverse_endpoints =
      endpoint_binding("z", "external_resource", "external_z", "source_z") +
      "," + endpoint_binding("a", "runtime_unit", "runtime_a", "source_a");

  const auto forward =
      compute_capability_provider_binding_revisions(decode(minimal_document(
          "[]",
          "[" +
              provider_binding("provider", kZeroDigest, "target_binding",
                               target_binding_target(), forward_endpoints) +
              "]")));
  const auto reverse =
      compute_capability_provider_binding_revisions(decode(minimal_document(
          "[]",
          "[" +
              provider_binding("provider", kOneDigest, "target_binding",
                               target_binding_target(), reverse_endpoints) +
              "]")));

  EXPECT_EQ(forward.value(), reverse.value());
  EXPECT_EQ(canonical_json(forward.normalized_capability_provider_bindings()),
            canonical_json(reverse.normalized_capability_provider_bindings()));
}

TEST(DeploymentApplicabilityCodec,
     OptionalExternalTablePreservesSourcePresence) {
  const auto absent = decode(minimal_document("[]", "[]", "[]", false));
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

  const auto exact =
      decode(minimal_document("[]", "[]", "[]", true, site_overlay_ref()));
  EXPECT_EQ(exact.site_overlay_ref_presence(),
            SiteOverlayRefSourcePresence::kValue);
  ASSERT_TRUE(exact.value().site_overlay_ref.has_value());
  EXPECT_EQ(exact.value().site_overlay_ref->site_overlay_id.value(), "plant_a");
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

  const auto verified = verify_capability_provider_binding_revisions(exact);
  EXPECT_EQ(verified.site_overlay_ref_presence(),
            SiteOverlayRefSourcePresence::kValue);
  EXPECT_EQ(verified.normalized_site_overlay_ref(),
            exact.normalized_site_overlay_ref());
}

TEST(DeploymentApplicabilityCodec,
     FocusedProjectionRequiresItsVersionAndTwoRequiredTables) {
  EXPECT_THROW((void)decode("{\"desired_control_configurations\":[],"
                            "\"capability_provider_bindings\":[]}"),
               DeploymentApplicabilityCodecError);
  EXPECT_THROW((void)decode("{\"api_version\":\"manyforge.deployment.v0\","
                            "\"desired_control_configurations\":[],"
                            "\"capability_provider_bindings\":[]}"),
               DeploymentApplicabilityCodecError);
  EXPECT_THROW((void)decode("{\"api_version\":\"manyforge.deployment.v1\","
                            "\"capability_provider_bindings\":[]}"),
               DeploymentApplicabilityCodecError);
  EXPECT_THROW((void)decode("{\"api_version\":\"manyforge.deployment.v1\","
                            "\"desired_control_configurations\":[]}"),
               DeploymentApplicabilityCodecError);
}

TEST(DeploymentApplicabilityCodec,
     ExactShapesAndClosedEnumsRejectMalformedRows) {
  const std::string valid_provider = provider_binding();
  EXPECT_THROW(
      (void)decode(minimal_document("[" +
                                        replace_first(desired_configuration(),
                                                      "\"binding_routes\"",
                                                      "\"unknown_routes\"") +
                                        "]",
                                    "[]")),
      DeploymentApplicabilityCodecError);
  EXPECT_THROW((void)decode(minimal_document(
                   "[]", "[" +
                             replace_first(valid_provider, "\"target_binding\"",
                                           "\"unknown_scope\"") +
                             "]")),
               DeploymentApplicabilityCodecError);
  EXPECT_THROW((void)decode(minimal_document(
                   "[]", "[" +
                             replace_first(valid_provider, "\"target\":{",
                                           "\"target\":{\"cell_id\":\"cell_a\","
                                           "\"unexpected\":\"x\",") +
                             "]")),
               DeploymentApplicabilityCodecError);
  EXPECT_THROW((void)decode(minimal_document(
                   "[]", "[" +
                             replace_first(valid_provider, "\"runtime_unit\"",
                                           "\"unknown_source\"") +
                             "]")),
               DeploymentApplicabilityCodecError);
  EXPECT_THROW(
      (void)decode(minimal_document(
          "[]", "[" +
                    replace_first(valid_provider, "\"planner_runtime\"",
                                  "\"bad/runtime\"") +
                    "]")),
      DeploymentApplicabilityCodecError);
  EXPECT_THROW(
      (void)decode(minimal_document(
          "[]", "[]", "[" + external_resource("alias", "unknown_kind") + "]")),
      DeploymentApplicabilityCodecError);
  EXPECT_THROW(
      (void)decode(minimal_document(
          "[]", "[]",
          "[" + external_resource("alias", "service_endpoint", "") + "]")),
      DeploymentApplicabilityCodecError);
  EXPECT_THROW((void)decode(minimal_document("[]", "[]", "null")),
               DeploymentApplicabilityCodecError);
  EXPECT_THROW(
      (void)decode(minimal_document("[]", "[]", "[]", true,
                                    "{\"site_overlay_id\":\"plant_a\","
                                    "\"site_overlay_revision\":\"" +
                                        std::string(kZeroDigest) + "\"}")),
      DeploymentApplicabilityCodecError);
  EXPECT_THROW((void)decode(minimal_document("[]", "[]", "[]", true,
                                             site_overlay_ref("bad/site"))),
               DeploymentApplicabilityCodecError);
  EXPECT_THROW((void)decode(minimal_document(
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
     RetainsAndCanonicallyOrdersClosedInterruptDeclarations) {
  const std::string providers = "[" + provider_binding("signal_z") + "," +
                                provider_binding("signal_a") + "]";
  const std::string sources = interrupt_source("source_z", "signal_z") + "," +
                              interrupt_source("source_a", "signal_a");
  const std::string policies =
      interrupt_policy("policy_z", "source_z",
                       "[\"intervention_required\",\"executing\"]",
                       "latch_faulted", "fault_terminal", "static",
                       "[\"domain_z\",\"domain_a\"]",
                       "[\"resource_z\",\"resource_a\"]") +
      "," +
      interrupt_policy(
          "policy_a", "source_a", "[\"stopping\",\"executing\"]",
          "complete_move_on_signal", "auto_clear_after_motion_outcome",
          "operation", "[\"domain_a\"]", "[\"resource_a\"]",
          ",\"allowed_activation_modes\":[\"trip_if_already_active\","
          "\"fresh_edge_after_arm\"],\"allowed_stop_modes\":["
          "\"hard_deceleration\",\"soft_on_path\"],"
          "\"scope_expiry\":\"operation_terminal\"");

  const auto candidate =
      decode(with_policies(minimal_document("[]", providers),
                           interrupt_policy_envelope(sources, policies)));
  ASSERT_TRUE(candidate.normalized_policies().has_value());
  const std::string normalized =
      canonical_json(*candidate.normalized_policies());
  EXPECT_LT(normalized.find("\"source_a\""), normalized.find("\"source_z\""));
  EXPECT_LT(normalized.find("\"policy_a\""), normalized.find("\"policy_z\""));
  EXPECT_NE(normalized.find("\"affected_phases\":[\"executing\",\"stopping\"]"),
            std::string::npos);
  EXPECT_NE(normalized.find("\"affected_phases\":[\"executing\","
                            "\"intervention_required\"]"),
            std::string::npos);
  EXPECT_NE(normalized.find("\"allowed_stop_modes\":[\"soft_on_path\","
                            "\"hard_deceleration\"]"),
            std::string::npos);
  EXPECT_NE(normalized.find("\"resource_ids\":[\"resource_a\",\"resource_z\"]"),
            std::string::npos);

  const auto revisioned =
      compute_capability_provider_binding_revisions(candidate);
  ASSERT_TRUE(revisioned.normalized_policies().has_value());
  EXPECT_EQ(canonical_json(*revisioned.normalized_policies()), normalized);
}

TEST(DeploymentApplicabilityCodec,
     RetainsOnlyCanonicalClosedPolicyCatalogReferences) {
  const std::string retained =
      "[" +
      catalog_record("stop_evidence_policy", "stop.alpha",
                     "manyforge.stop_evidence_policy.v2") +
      "," +
      catalog_record("contact_policy", "contact.alpha",
                     "manyforge.contact_policy.v1") +
      "]";

  const auto candidate = decode(with_policies(
      minimal_document(),
      interrupt_policy_envelope("", "", interrupt_config(), retained)));
  const auto &references = candidate.value().retained_policy_records;
  ASSERT_EQ(references.size(), 2U);
  EXPECT_TRUE(std::holds_alternative<core::ContactPolicyCatalogRecordRefDTO>(
      references[0]));
  EXPECT_TRUE(
      std::holds_alternative<core::StopEvidencePolicyCatalogRecordRefDTO>(
          references[1]));

  ASSERT_TRUE(candidate.normalized_policies().has_value());
  const std::string normalized =
      canonical_json(*candidate.normalized_policies());
  EXPECT_LT(normalized.find("\"contact_policy\""),
            normalized.find("\"stop_evidence_policy\""));

  const auto revisioned =
      compute_capability_provider_binding_revisions(candidate);
  EXPECT_EQ(revisioned.value().retained_policy_records, references);
  ASSERT_TRUE(revisioned.normalized_policies().has_value());
  EXPECT_EQ(canonical_json(*revisioned.normalized_policies()), normalized);
}

TEST(DeploymentApplicabilityCodec,
     RejectsNonPolicyAndDuplicateRetainedPolicyReferences) {
  const auto document_for = [](std::string records) {
    return with_policies(
        minimal_document(),
        interrupt_policy_envelope("", "", interrupt_config(),
                                  "[" + std::move(records) + "]"));
  };
  expect_decode_error_containing(
      document_for(catalog_record("component_type", "arm",
                                  "manyforge.component_type.v1")),
      "outside the closed policy-family set");
  expect_decode_error_containing(
      document_for(catalog_record("runtime_policy", "runtime.alpha",
                                  "manyforge.runtime_policy.v1")),
      "outside the closed v1 catalog registry");

  const std::string duplicate = catalog_record(
      "contact_policy", "contact.alpha", "manyforge.contact_policy.v1");
  expect_decode_error_containing(document_for(duplicate + "," + duplicate),
                                 "duplicate complete catalog-record identity");
}

TEST(DeploymentApplicabilityCodec,
     InterruptDeclarationsRejectBrokenIdentityAndPolicyJoins) {
  const std::string provider = "[" + provider_binding("signal") + "]";
  const auto document_for = [&](std::string sources, std::string policies) {
    return with_policies(
        minimal_document("[]", provider),
        interrupt_policy_envelope(std::move(sources), std::move(policies)));
  };
  expect_decode_error_containing(
      document_for(interrupt_source("source", "missing"),
                   interrupt_policy("policy", "source")),
      "references an unknown capability-provider binding");
  expect_decode_error_containing(
      document_for(interrupt_source("source", "signal") + "," +
                       interrupt_source("source", "signal"),
                   interrupt_policy("policy", "source")),
      "duplicate source_id");
  expect_decode_error_containing(
      document_for(interrupt_source("source", "signal"),
                   interrupt_policy("policy", "missing")),
      "references an unknown interrupt source");
  expect_decode_error_containing(
      document_for(interrupt_source("source", "signal"),
                   interrupt_policy("policy", "source", "[\"executing\"]",
                                    "preempt_trajectory",
                                    "await_recovery_action", "static", "[]",
                                    "[]")),
      "requires at least one control domain or resource");
  expect_decode_error_containing(
      document_for(interrupt_source("source", "signal"),
                   interrupt_policy("policy", "source", "[\"planning\"]",
                                    "preempt_trajectory",
                                    "await_recovery_action", "operation")),
      "operation policy must include executing");
  // Operation-level Pause was removed; its former phase is refused, not
  // aliased.
  expect_decode_error_containing(
      document_for(interrupt_source("source", "signal"),
                   interrupt_policy("policy", "source",
                                    "[\"executing\",\"paused\"]")),
      "value is outside the closed enum");
  expect_decode_error_containing(
      document_for(interrupt_source("source", "signal"),
                   interrupt_policy("policy_a", "source", "[\"executing\"]",
                                    "preempt_trajectory",
                                    "await_recovery_action") +
                       "," +
                       interrupt_policy("policy_b", "source", "[\"executing\"]",
                                        "latch_faulted", "fault_terminal")),
      "same-source equal-priority overlapping phases");
  expect_decode_error_containing(
      document_for(interrupt_source("source", "signal"),
                   interrupt_policy("policy", "source", "[\"executing\"]",
                                    "preempt_trajectory",
                                    "await_recovery_action", "static",
                                    "[\"arm_domain\"]", "[]",
                                    ",\"maskable_by\":[\"missing\"]")),
      "references an unknown interrupt policy");
}

TEST(DeploymentApplicabilityCodec,
     InterruptConfigurationUsesExactQualifiedNumericBounds) {
  const std::string provider = "[" + provider_binding("signal") + "]";
  const std::string source = interrupt_source("source", "signal");
  const std::string policy = interrupt_policy("policy", "source");
  const auto document_for = [&](std::string config) {
    return with_policies(
        minimal_document("[]", provider),
        interrupt_policy_envelope(source, policy, std::move(config)));
  };
  expect_decode_error_containing(
      document_for(interrupt_config("1.5")),
      "queue_depth: expected a non-negative I-JSON-safe integer");
  expect_decode_error_containing(
      document_for(interrupt_config("64", "0")),
      "cancel_validation_timeout_ns: integer is outside");
  expect_decode_error_containing(
      document_for(interrupt_config("64", "100", "200", "10.1")),
      "supervisory_poll_rate_hz: must be within");
  expect_decode_error_containing(
      document_for(replace_first(interrupt_config(), "\"queue_depth\"",
                                 "\"unknown_depth\"")),
      "unknown field 'unknown_depth'");
}

TEST(DeploymentApplicabilityCodec,
     RetainsClosedStructuredHysteresisAndTypedStopProfile) {
  const std::string provider = "[" + provider_binding("signal") + "]";
  const std::string profile =
      "{\"profile_id\":\"guard.soft\",\"stop_mode\":\"soft_on_path\","
      "\"jerk_limits_enabled\":true,"
      "\"deceleration_time_s\":1.25,\"per_joint_max_deceleration\":{"
      "\"z_slide\":{\"coordinate_kind\":\"linear_m_s2\",\"value_m_s2\":2.5},"
      "\"a_joint\":{\"coordinate_kind\":\"angular_rad_s2\","
      "\"value_rad_s2\":3.5}},\"recoverable_substitution_mode\":"
      "\"spring_back_hold\",\"per_joint_settle_velocity_threshold\":{"
      "\"z_slide\":{\"coordinate_kind\":\"linear_m_s\",\"value_m_s\":0.01}},"
      "\"diagnostics\":{\"qualified\":true,\"revision\":\"v2\","
      "\"tolerance\":0.5}}";
  const auto candidate = decode(with_policies(
      minimal_document("[]", provider),
      interrupt_policy_envelope(
          interrupt_source(
              "source", "signal", "fast_execution_relevant", "level", "100",
              ",\"hysteresis_value\":{\"high\":2,\"latched\":false}"),
          interrupt_policy("policy", "source", "[\"executing\"]",
                           "preempt_trajectory", "await_recovery_action",
                           "static", "[\"arm_domain\"]", "[]",
                           ",\"stop_profile\":" + profile))));
  ASSERT_TRUE(candidate.normalized_policies().has_value());
  const std::string normalized =
      canonical_json(*candidate.normalized_policies());
  EXPECT_NE(normalized.find("\"allow_natural_completion_on_short_path\":true"),
            std::string::npos);
  EXPECT_NE(normalized.find("\"require_backend_confirmation\":true"),
            std::string::npos);
  EXPECT_LT(normalized.find("\"a_joint\""), normalized.find("\"z_slide\""));
  EXPECT_NE(normalized.find("\"hysteresis_value\":{\"high\":2,"
                            "\"latched\":false}"),
            std::string::npos);
}

TEST(DeploymentApplicabilityCodec,
     TypedInterruptValuesRejectOpenShapesAndRetiredProfileFields) {
  const std::string provider = "[" + provider_binding("signal") + "]";
  const auto document_for_source = [&](std::string optional_fields) {
    return with_policies(
        minimal_document("[]", provider),
        interrupt_policy_envelope(
            interrupt_source("source", "signal", "fast_execution_relevant",
                             "level", "100", optional_fields),
            interrupt_policy("policy", "source")));
  };
  const auto document_for_profile = [&](std::string profile) {
    return with_policies(
        minimal_document("[]", provider),
        interrupt_policy_envelope(
            interrupt_source("source", "signal"),
            interrupt_policy("policy", "source", "[\"executing\"]",
                             "preempt_trajectory", "await_recovery_action",
                             "static", "[\"arm_domain\"]", "[]",
                             ",\"stop_profile\":" + profile)));
  };

  expect_decode_error_containing(
      document_for_source(",\"hysteresis_value\":{}"),
      "must contain 1..64 entries");
  expect_decode_error_containing(
      document_for_source(",\"hysteresis_value\":{\"nested\":{\"x\":1}}"),
      "must be exactly boolean, finite number, or NFC string");
  expect_decode_error_containing(
      document_for_profile(
          "{\"profile_id\":\"soft\",\"stop_mode\":\"soft_on_path\","
          "\"deceleration_time_s\":1.0}"),
      "requires its exact duration/limit presence matrix");
  expect_decode_error_containing(
      document_for_profile(
          "{\"profile_id\":\"hard\",\"stop_mode\":\"hard_deceleration\","
          "\"per_joint_max_deceleration\":{\"joint\":{"
          "\"coordinate_kind\":\"angular_rad_s2\",\"value_rad_s2\":1}}}"),
      "hard_deceleration forbids");
  expect_decode_error_containing(
      document_for_profile(
          "{\"profile_id\":\"spring\",\"stop_mode\":\"spring_back_hold\","
          "\"jerk_limits_enabled\":false,"
          "\"per_joint_max_deceleration\":{\"joint\":{"
          "\"coordinate_kind\":\"angular_rad_s2\",\"value_m_s2\":1}}}"),
      "unknown field 'value_m_s2'");
  EXPECT_NO_THROW((void)decode(document_for_profile(
      "{\"profile_id\":\"spring\",\"stop_mode\":\"spring_back_hold\","
      "\"jerk_limits_enabled\":false,"
      "\"per_joint_max_deceleration\":{\"joint\":{"
      "\"coordinate_kind\":\"angular_rad_s2\",\"value_rad_s2\":1}},"
      "\"allow_natural_completion_on_short_path\":true}")));
  expect_decode_error_containing(
      document_for_profile(
          "{\"profile_id\":\"hard\",\"stop_mode\":\"hard_deceleration\","
          "\"allow_natural_completion_on_short_path\":false}"),
      "explicit presence is legal only for soft_on_path or spring_back_hold");
  expect_decode_error_containing(
      document_for_profile(
          "{\"profile_id\":\"soft\",\"stop_mode\":\"soft_on_path\","
          "\"deceleration_time_s\":1,\"per_joint_max_deceleration\":{"
          "\"joint\":{\"coordinate_kind\":\"angular_rad_s2\","
          "\"value_rad_s2\":1}},\"allowed_fallback_modes\":[]}"),
      "unknown field 'allowed_fallback_modes'");
}

TEST(DeploymentApplicabilityCodec,
     DuplicateKeysRejectAtEveryOwnedCollectionBoundary) {
  const std::string route = desired_route();
  EXPECT_THROW((void)decode(minimal_document(
                   "[" +
                       desired_configuration("domain", kZeroDigest,
                                             route + "," + route) +
                       "]",
                   "[]")),
               DeploymentApplicabilityCodecError);

  const std::string endpoint = endpoint_binding();
  EXPECT_THROW(
      (void)decode(minimal_document(
          "[]", "[" +
                    provider_binding("provider", kZeroDigest, "target_binding",
                                     target_binding_target(),
                                     endpoint + "," + endpoint) +
                    "]")),
      DeploymentApplicabilityCodecError);

  const std::string desired = desired_configuration();
  EXPECT_THROW(
      (void)decode(minimal_document("[" + desired + "," + desired + "]", "[]")),
      DeploymentApplicabilityCodecError);

  const std::string provider = provider_binding();
  EXPECT_THROW((void)decode(minimal_document("[]", "[" + provider + "," +
                                                       provider + "]")),
               DeploymentApplicabilityCodecError);

  const std::string external = external_resource();
  EXPECT_THROW((void)decode(minimal_document(
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
      first_256 +=
          endpoint_binding("role_" + std::to_string(index), "runtime_unit",
                           "runtime", "source_" + std::to_string(index));
    }
  }
  const std::string endpoints =
      first_256 + "," +
      endpoint_binding("role_256", "runtime_unit", "runtime", "source_256");
  EXPECT_NO_THROW((void)decode(minimal_document(
      "[]", "[" +
                provider_binding("provider", kZeroDigest, "target_binding",
                                 target_binding_target(), first_256) +
                "]")));
  EXPECT_THROW(
      (void)decode(minimal_document(
          "[]", "[" +
                    provider_binding("provider", kZeroDigest, "target_binding",
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
  EXPECT_THROW((void)compute_capability_provider_binding_revisions(candidate),
               DeploymentApplicabilityCodecError);
}

TEST(DeploymentApplicabilityCodec,
     SiteOverlayValueRefsUseTheExactPortableV1Grammar) {
  const std::string maximum_ref = "a" + std::string(127U, 'z');
  const auto candidate = decode(minimal_document(
      "[]", "[]",
      "[" + external_resource("endpoint", "service_endpoint", maximum_ref) +
          "]"));
  ASSERT_EQ(candidate.value().external_resources.size(), 1U);
  EXPECT_EQ(
      candidate.value().external_resources[0].site_overlay_value_ref.value(),
      maximum_ref);

  for (const std::string_view invalid :
       {"", ".leading", "resources/site", "resources site", "resources[0]",
        "resources:site"}) {
    EXPECT_THROW(
        (void)decode(minimal_document(
            "[]", "[]",
            "[" + external_resource("endpoint", "service_endpoint", invalid) +
                "]")),
        DeploymentApplicabilityCodecError)
        << invalid;
  }
  EXPECT_THROW((void)decode(minimal_document(
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
  EXPECT_EQ(
      candidate.value().external_resources[0].external_resource_alias.value(),
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
        minimal_document("[]", "[]", table_for_alias(invalid)), kAliasPath);
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
  EXPECT_NO_THROW(
      (void)decode(minimal_document("[]", "[]", "[" + first_4096 + "]")));
  EXPECT_THROW((void)decode(minimal_document(
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
        return desired_configuration("domain_" + std::to_string(index),
                                     kZeroDigest, "");
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
      minimal_document("[]", "[" + boundary_rows + "," +
                                 provider_binding("provider_4096") + "]"),
      "$.capability_provider_bindings: provider-binding count exceeds");
}

TEST(DeploymentApplicabilityCodec,
     DesiredBindingRouteCeilingAccepts4096AndRejects4097) {
  const std::string boundary_rows = comma_joined_rows(
      core::kMaxDeploymentApplicabilityRows, [](std::size_t index) {
        return desired_route("binding_" + std::to_string(index), kZeroDigest,
                             "route_" + std::to_string(index));
      });

  const auto accepted = decode(minimal_document(
      "[" + desired_configuration("domain", kZeroDigest, boundary_rows) + "]",
      "[]"));
  ASSERT_EQ(accepted.value().desired_control_configurations.size(), 1U);
  EXPECT_EQ(
      accepted.value().desired_control_configurations[0].binding_routes.size(),
      core::kMaxDeploymentApplicabilityRows);

  expect_decode_error_containing(
      minimal_document("[" +
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

TEST(DeploymentApplicabilityCodec, PerBindingStopTimingIsNoLongerAPolicyField) {
  // Q51/Q53: each robot's Stop timing lives in its own controlled-Stop profile
  // (`max_stop_time_s`); the cell-wide per-binding timing table is gone and
  // an older document carrying it is refused, not converted.
  auto policies = interrupt_policy_envelope("", "");
  const auto absent = decode(with_policies(minimal_document(), policies));
  EXPECT_EQ(absent.normalized_policies()->find_object_member(
                "stop_timing_parameters"),
            nullptr);
  auto with_timing = policies;
  with_timing.pop_back();
  expect_decode_error_containing(
      with_policies(minimal_document(),
                    with_timing +
                        ",\"stop_timing_parameters\":[{\"control_binding_id\":\"a\","
                        "\"control_binding_revision\":\"" +
                        std::string(kZeroDigest) +
                        "\",\"timing\":{\"command_latency_allowance_ns\":250000000,"
                        "\"settling_allowance_ns\":750000000,"
                        "\"operational_timeout_ns\":3000000000}}]}"),
      "unknown field");
}

TEST(DeploymentApplicabilityCodec,
     MotionTakeoverPlanningBudgetRequiresAStrictlyLargerPreparationBudget) {
  const auto document_for = [](std::string_view planning,
                               bool preparation = true) {
    auto policies = interrupt_policy_envelope("", "");
    policies.pop_back();
    if (preparation)
      policies += ",\"motion_takeover_preparation_budget_s\":3.5";
    policies +=
        ",\"motion_takeover_planning_budget_s\":" + std::string(planning) + "}";
    return with_policies(minimal_document(), policies);
  };
  const auto ordinary = decode(
      with_policies(minimal_document(), interrupt_policy_envelope("", "")));
  EXPECT_FALSE(ordinary.value().motion_takeover_planning_budget_s);
  EXPECT_EQ(ordinary.normalized_policies()->find_object_member(
                "motion_takeover_planning_budget_s"),
            nullptr);
  const auto candidate = decode(document_for("1.25"));
  EXPECT_EQ(candidate.value().motion_takeover_preparation_budget_s,
            std::optional<double>(3.5));
  EXPECT_EQ(candidate.value().motion_takeover_planning_budget_s,
            std::optional<double>(1.25));
  const auto sealed = compute_capability_provider_binding_revisions(candidate);
  EXPECT_EQ(sealed.value().motion_takeover_planning_budget_s,
            std::optional<double>(1.25));
  EXPECT_EQ(canonical_json(*sealed.normalized_policies()),
            canonical_json(*candidate.normalized_policies()));
  for (const auto *invalid : {"0", "-1", "3.5", "4", "null", "true", "\"1\""})
    expect_decode_error_containing(
        document_for(invalid), "$.policies.motion_takeover_planning_budget_s");
  expect_decode_error_containing(
      document_for("1", false), "$.policies.motion_takeover_planning_budget_s");
}

TEST(DeploymentApplicabilityCodec, ScenePosePoliciesAreClosedAndRetained) {
  const auto document_for = [](const std::string &rows) {
    auto policies = interrupt_policy_envelope("", "");
    policies.pop_back();
    return with_policies(minimal_document(),
                         policies + ",\"scene_pose_sources\":" + rows + "}");
  };
  for (const auto *policy :
       {"observe_only", "continuous", "continuous_unattached",
        "continuous_relative_to_attached", "before_planning",
        "between_nodes"}) {
    const auto row =
        std::string(
            R"({"source_id":"camera","scene_resource_id":"part",)"
            R"("source_key":{"simulation_entity":{"entity":"/World/part",)"
            R"("source_header_frame_id":"part"}},)"
            R"("apply_policy":")") +
        policy + R"("})";
    const auto candidate = decode(document_for("[" + row + "]"));
    ASSERT_EQ(candidate.value().scene_pose_sources.size(), 1U);
    EXPECT_EQ(candidate.value().scene_pose_sources.front().apply_policy,
              policy);
    const auto revisioned =
        compute_capability_provider_binding_revisions(candidate);
    EXPECT_EQ(revisioned.value().scene_pose_sources,
              candidate.value().scene_pose_sources);
    EXPECT_EQ(revisioned.normalized_policies(),
              candidate.normalized_policies());
    EXPECT_THROW(decode(document_for("[" + row + "," + row + "]")),
                 std::invalid_argument);
  }
  EXPECT_THROW(
      decode(document_for(
          R"([{"source_id":"camera","scene_resource_id":"part",)"
          R"("source_key":{"simulation_entity":{"entity":"/World/part",)"
            R"("source_header_frame_id":"part"}},)"
          R"("apply_policy":"when_idle"}])")),
      std::invalid_argument);
}

TEST(DeploymentApplicabilityCodec, ScenePoseSourceKeysRoundTripOneRowPerObject) {
  const auto document_for = [](const std::string &rows) {
    auto policies = interrupt_policy_envelope("", "");
    policies.pop_back();
    return with_policies(minimal_document(),
                         policies + ",\"scene_pose_sources\":" + rows + "}");
  };
  const std::string entity =
      R"({"source_id":"sim","scene_resource_id":"box","source_key":)"
      R"({"simulation_entity":{"entity":"/World/box","source_header_frame_id":"box",)"
      R"("local_position_xyz":[0,0,0.1],"local_orientation_xyzw":[0,0,0,1]}},)"
      R"("apply_policy":"continuous"})";
  const std::string detection =
      R"({"source_id":"camera","scene_resource_id":"part","source_key":)"
      R"({"detection3d":{"class_id":"part","detection_id":"",)"
      R"("confidence_min":0.5,"max_position_variance_m2":0.0025,)"
      R"("selection":"first"}},"apply_policy":"before_planning"})";
  const auto candidate =
      decode(document_for("[" + entity + "," + detection + "]"));
  const std::vector<core::ScenePoseSourcePolicyDTO> expected{
      {"camera", "part",
       core::ScenePoseDetection3dKeyDTO{"part", "", 0.5, 0.0025, "first"},
       "before_planning", 0.0, 0.0},
      {"sim", "box",
       core::ScenePoseSimulationEntityKeyDTO{
           "/World/box", "box", {0.0, 0.0, 0.1}, {0.0, 0.0, 0.0, 1.0}},
       "continuous", 0.0, 0.0},
  };
  EXPECT_EQ(candidate.value().scene_pose_sources, expected);
  // Canonical: identity orientation is absent; the codec round-trips.
  const auto *rows =
      candidate.normalized_policies()->find_object_member("scene_pose_sources");
  ASSERT_NE(rows, nullptr);
  EXPECT_EQ(canonical_json(*rows).find("local_orientation_xyzw"),
            std::string::npos);
  EXPECT_EQ(DecodeScenePoseSources(*rows), expected);
  EXPECT_EQ(EncodeScenePoseSources(expected), *rows);
  const auto revisioned =
      compute_capability_provider_binding_revisions(candidate);
  EXPECT_EQ(revisioned.value().scene_pose_sources, expected);

  const auto refused = [&](const std::string &rows, std::string_view text) {
    try {
      (void)decode(document_for(rows));
      ADD_FAILURE() << "accepted " << rows;
    } catch (const std::invalid_argument &error) {
      EXPECT_NE(std::string(error.what()).find(text), std::string::npos)
          << error.what();
    }
  };
  // One source per object: a second row for "box", even from another
  // source, is refused.
  auto other_source = entity;
  other_source.replace(other_source.find("\"sim\""), 5U, "\"cam\"");
  refused("[" + entity + "," + other_source + "]",
          "scene_pose_source_duplicate_object");
  const auto keyed = [](const std::string &key) {
    return R"([{"source_id":"sim","scene_resource_id":"box","source_key":)" +
           key + R"(,"apply_policy":"continuous"}])";
  };
  refused(keyed(R"({})"), "exactly one member");
  refused(keyed(R"({"simulation_entity":{"entity":"/a","source_header_frame_id":"a"},)"
                R"("detection3d":{"class_id":"","detection_id":"",)"
                R"("confidence_min":0,"max_position_variance_m2":0,)"
                R"("selection":""}})"),
          "exactly one member");
  refused(keyed(R"({"camera":{}})"), "unsupported scene pose-source source_key");
  refused(keyed(R"({"simulation_entity":{"entity":"","source_header_frame_id":"a"}})"),
          "entity");
  refused(keyed(R"({"simulation_entity":{"entity":"/a"}})"),
          "source_header_frame_id");
  refused(keyed(R"({"simulation_entity":{"entity":"/a","source_header_frame_id":"a",)"
                R"("link":"a"}})"),
          "unknown fields");
  refused(keyed(R"({"simulation_entity":{"entity":"/a","source_header_frame_id":"a",)"
                R"("local_position_xyz":[0,0]}})"),
          "local_position_xyz");
  refused(keyed(R"({"detection3d":{"class_id":"","detection_id":"",)"
                R"("confidence_min":0,"max_position_variance_m2":0}})"),
          "selection");
  refused(keyed(R"({"detection3d":{"class_id":"","detection_id":"",)"
                R"("confidence_min":0,"max_position_variance_m2":0,)"
                R"("selection":"last"}})"),
          "selection");
  refused(R"([{"source_id":"sim","scene_resource_id":"box",)"
          R"("apply_policy":"continuous"}])",
          "source_key");
}

TEST(DeploymentApplicabilityCodec,
     ComponentStreamOverridesAreClosedNormalizedAndRetained) {
  const auto document_for = [](const std::string &rows) {
    auto policies = interrupt_policy_envelope("", "");
    policies.pop_back();
    return with_policies(minimal_document(),
                         policies + ",\"component_streams\":" + rows + "}");
  };
  // Authored out of canonical order; the codec normalizes the table.
  const auto candidate = decode(document_for(
      R"([{"controllable_component_id":"robot_1","stream_id":"measured_joints","budget_ns":300000000,"recovery":"auto"},)"
      R"({"controllable_component_id":"octomap_1","stream_id":"map_updates","severity":"fault"}])"));
  const std::vector<core::StreamParameterOverrideDTO> expected{
      {core::ControllableComponentId("octomap_1"), "map_updates", std::nullopt,
       core::StreamSeverityDTO::kFault, std::nullopt},
      {core::ControllableComponentId("robot_1"), "measured_joints",
       std::int64_t{300000000}, std::nullopt, core::StreamRecoveryDTO::kAuto},
  };
  ASSERT_TRUE(candidate.normalized_policies().has_value());
  const auto *streams =
      candidate.normalized_policies()->find_object_member("component_streams");
  ASSERT_NE(streams, nullptr);
  EXPECT_EQ(decode_component_stream_overrides(*streams), expected);
  EXPECT_EQ(*streams, encode_component_stream_overrides(expected));
  const auto revisioned =
      compute_capability_provider_binding_revisions(candidate);
  EXPECT_EQ(revisioned.normalized_policies(), candidate.normalized_policies());

  // Absent or empty tables keep the existing policy projection unchanged.
  const auto ordinary = decode(
      with_policies(minimal_document(), interrupt_policy_envelope("", "")));
  EXPECT_EQ(ordinary.normalized_policies()->find_object_member(
                "component_streams"),
            nullptr);
  EXPECT_EQ(decode(document_for("[]")).normalized_policies(),
            ordinary.normalized_policies());

  // `fault` + `auto` is accepted (the user's choice of safety level).
  EXPECT_NO_THROW(decode(document_for(
      R"([{"controllable_component_id":"octomap_1","stream_id":"map_updates","severity":"fault","recovery":"auto"}])")));

  const std::string row_prefix =
      R"({"controllable_component_id":"octomap_1","stream_id":"map_updates")";
  expect_decode_error_containing(
      document_for("[" + row_prefix + R"(,"severity":"fault"},)" + row_prefix +
                   R"(,"budget_ns":5}])"),
      "$.policies.component_streams: duplicate");
  expect_decode_error_containing(document_for("[" + row_prefix + "}]"),
                                 "$.policies.component_streams[0]: expected "
                                 "at least one");
  expect_decode_error_containing(
      document_for("[" + row_prefix + R"(,"severity":"fatal"}])"),
      "$.policies.component_streams[0].severity");
  expect_decode_error_containing(
      document_for("[" + row_prefix + R"(,"recovery":"never"}])"),
      "$.policies.component_streams[0].recovery");
  expect_decode_error_containing(
      document_for("[" + row_prefix + R"(,"budget_ns":0}])"),
      "$.policies.component_streams[0].budget_ns");
  expect_decode_error_containing(
      document_for("[" + row_prefix + R"(,"timestamp_source":"arrival"}])"),
      "unknown field 'timestamp_source'");
  expect_decode_error_containing(
      document_for(
          R"([{"controllable_component_id":"octomap_1","stream_id":"Map","severity":"fault"}])"),
      "$.policies.component_streams[0].stream_id");
  expect_decode_error_containing(document_for("{}"),
                                 "$.policies.component_streams: expected "
                                 "array");
}

TEST(DeploymentApplicabilityCodec, ScenePoseDeadbandIsOptionalNonnegative) {
  const auto document_for = [](const std::string &extra) {
    auto policies = interrupt_policy_envelope("", "");
    policies.pop_back();
    return with_policies(
        minimal_document(),
        policies +
            R"(,"scene_pose_sources":[{"source_id":"camera","scene_resource_id":"part",)"
            R"("source_key":{"simulation_entity":{"entity":"/World/part",)"
            R"("source_header_frame_id":"part"}},)"
            R"("apply_policy":"continuous")" +
            extra + "}]}");
  };
  // Default 0, and a zero deadband stays absent from the normalized form.
  for (const auto *absent : {"", R"(,"deadband_translation_m":0)"}) {
    const auto plain = decode(document_for(absent));
    const auto &row = plain.value().scene_pose_sources.front();
    EXPECT_EQ(row.deadband_translation_m, 0.0);
    EXPECT_EQ(row.deadband_rotation_rad, 0.0);
    EXPECT_EQ(canonical_json(*plain.normalized_policies())
                  .find("deadband"),
              std::string::npos);
  }
  const auto banded = decode(document_for(
      R"(,"deadband_translation_m":0.0001,"deadband_rotation_rad":0.0002)"));
  const auto &row = banded.value().scene_pose_sources.front();
  EXPECT_DOUBLE_EQ(row.deadband_translation_m, 0.0001);
  EXPECT_DOUBLE_EQ(row.deadband_rotation_rad, 0.0002);
  const auto revisioned = compute_capability_provider_binding_revisions(banded);
  EXPECT_EQ(revisioned.value().scene_pose_sources,
            banded.value().scene_pose_sources);
  EXPECT_EQ(revisioned.normalized_policies(), banded.normalized_policies());
  EXPECT_NE(canonical_json(*banded.normalized_policies())
                .find(R"("deadband_rotation_rad":0.0002)"),
            std::string::npos);
  for (const auto *invalid :
       {R"(,"deadband_translation_m":-0.1)", R"(,"deadband_rotation_rad":"1")",
        R"(,"deadband_rotation_rad":null)", R"(,"deadband":0.1)"})
    EXPECT_THROW(decode(document_for(invalid)), std::invalid_argument);
}

TEST(DeploymentApplicabilityCodec,
     MotionTakeoverBudgetIsOptionalPositiveAndPreserved) {
  const auto document_for = [](std::string_view value) {
    auto policies = interrupt_policy_envelope("", "");
    policies.pop_back();
    return with_policies(minimal_document(),
                         policies +
                             ",\"motion_takeover_preparation_budget_s\":" +
                             std::string(value) + "}");
  };
  const auto no_policies = decode(minimal_document());
  EXPECT_FALSE(
      no_policies.value().motion_takeover_preparation_budget_s.has_value());
  const auto omitted = decode(
      with_policies(minimal_document(), interrupt_policy_envelope("", "")));
  EXPECT_FALSE(
      omitted.value().motion_takeover_preparation_budget_s.has_value());
  EXPECT_EQ(omitted.normalized_policies()->find_object_member(
                "motion_takeover_preparation_budget_s"),
            nullptr);
  for (const auto *seconds : {"0.001", "1", "2.5"}) {
    const auto candidate = decode(document_for(seconds));
    ASSERT_TRUE(
        candidate.value().motion_takeover_preparation_budget_s.has_value());
    EXPECT_DOUBLE_EQ(*candidate.value().motion_takeover_preparation_budget_s,
                     std::stod(seconds));
    const auto revisioned =
        compute_capability_provider_binding_revisions(candidate);
    EXPECT_EQ(revisioned.value().motion_takeover_preparation_budget_s,
              candidate.value().motion_takeover_preparation_budget_s);
    EXPECT_EQ(canonical_json(*revisioned.normalized_policies()),
              canonical_json(*candidate.normalized_policies()));
  }
  for (const auto *invalid : {"0", "-1", "true", "null", "\"1\""}) {
    expect_decode_error_containing(
        document_for(invalid),
        "$.policies.motion_takeover_preparation_budget_s");
  }
}

} // namespace

TEST(DeploymentApplicabilityCodec, DirectComponentTargetIsDistinctClosedAndIdentitySensitive) {
  const std::string direct = "{\"controllable_component_id\":\"arm_component\","
      "\"component_type_revision\":\"" + std::string(kZeroDigest) + "\","
      "\"command_target_id\":\"gpio\"}";
  const auto document = minimal_document("[]", "[" + provider_binding("io.read", kZeroDigest,
      "target_binding", direct) + "]");
  const auto typed = decode(document);
  ASSERT_EQ(typed.value().capability_provider_bindings.size(), 1U);
  EXPECT_TRUE(std::holds_alternative<core::ComponentTargetCapabilityProviderTargetDTO>(
      typed.value().capability_provider_bindings.front().target));
  const auto verified = compute_capability_provider_binding_revisions(typed);
  const auto reparsed = decode(minimal_document("[]",
      canonical_json(verified.normalized_capability_provider_bindings())));
  EXPECT_EQ(reparsed.value().capability_provider_bindings, verified.value().capability_provider_bindings);
  const auto old = decode(minimal_document("[]", "[" + provider_binding() + "]"));
  EXPECT_TRUE(std::holds_alternative<core::TargetBindingCapabilityProviderTargetDTO>(
      old.value().capability_provider_bindings.front().target));
  EXPECT_NE(canonical_json(typed.normalized_capability_provider_bindings()),
            canonical_json(old.normalized_capability_provider_bindings()));
  for (const auto* extra : {",\"control_binding_id\":\"fake\"", ",\"unknown\":true"}) {
    auto invalid_target = direct;
    invalid_target.pop_back();
    invalid_target += std::string(extra) + "}";
    EXPECT_THROW(static_cast<void>(decode(minimal_document("[]", "[" + provider_binding(
        "io.read", kZeroDigest, "target_binding", invalid_target) + "]"))), DeploymentApplicabilityCodecError);
  }
}


TEST(DeploymentApplicabilityCodecTest, GlobalConditionUsesOnlyExistingNodeAndDepthBounds) {
  constexpr std::size_t maximum_nodes = 64U;
  const auto comparison = [](std::size_t index) {
    return CanonicalValue(CanonicalValue::Object{{"compare", CanonicalValue(CanonicalValue::Object{
        {"operand", CanonicalValue(CanonicalValue::Object{{"signal", CanonicalValue(CanonicalValue::Object{
            {"controllable_component_id", CanonicalValue("device")},
            {"signal_name", CanonicalValue("input." + std::to_string(index))}})}})},
        {"op", CanonicalValue("==")}, {"value", CanonicalValue(true)}})}});
  };
  const auto document = [&](const CanonicalValue& condition) {
    const CanonicalValue rule(CanonicalValue::Object{
        {"rule_id", CanonicalValue("many_sources")}, {"condition", condition},
        {"scope", CanonicalValue("all_motion")}});
    auto policies = interrupt_policy_envelope("", "");
    policies.pop_back();
    policies += ",\"interrupt_rules\":[" + canonical_json(rule) + "]}";
    return with_policies(minimal_document(), std::move(policies));
  };
  // A single comparison remains legal without an otherwise unnecessary parent.
  EXPECT_NO_THROW(static_cast<void>(decode(document(comparison(0U)))));
  CanonicalValue::Array children;
  for (std::size_t index = 0; index < maximum_nodes - 1U; ++index)
    children.push_back(comparison(index));
  const CanonicalValue largest(CanonicalValue::Object{{"all", CanonicalValue(children)}});
  const auto decoded = decode(document(largest));
  ASSERT_TRUE(decoded.normalized_policies());
  const auto* rules = decoded.normalized_policies()->find_object_member("interrupt_rules");
  ASSERT_NE(rules, nullptr);
  ASSERT_EQ(rules->as_array().size(), 1U);
  EXPECT_EQ(*rules->as_array().front().find_object_member("condition"), largest);
  // Aliasing every operand does not alter the authored node bound either.
  CanonicalValue::Array aliases(maximum_nodes - 1U, comparison(0U));
  EXPECT_NO_THROW(static_cast<void>(decode(document(CanonicalValue(
      CanonicalValue::Object{{"any", CanonicalValue(aliases)}})))));
  children.push_back(comparison(maximum_nodes - 1U));
  EXPECT_THROW(static_cast<void>(decode(document(CanonicalValue(
      CanonicalValue::Object{{"all", CanonicalValue(children)}})))), DeploymentApplicabilityCodecError);
  auto deepest = comparison(0U);
  for (std::size_t depth = 1U; depth < 8U; ++depth)
    deepest = CanonicalValue(CanonicalValue::Object{{"not", deepest}});
  EXPECT_NO_THROW(static_cast<void>(decode(document(deepest))));
  deepest = CanonicalValue(CanonicalValue::Object{{"not", deepest}});
  EXPECT_THROW(static_cast<void>(decode(document(deepest))), DeploymentApplicabilityCodecError);
}

TEST(DeploymentApplicabilityCodecTest, GlobalRuleOnTripIsGateByDefaultAndOwnsIdleBehaviour) {
  const auto document = [&](CanonicalValue::Object extra) {
    CanonicalValue::Object fields{
        {"rule_id", CanonicalValue("light_curtain")},
        {"condition", CanonicalValue(CanonicalValue::Object{{"compare", CanonicalValue(
            CanonicalValue::Object{{"operand", CanonicalValue(CanonicalValue::Object{
                {"signal", CanonicalValue(CanonicalValue::Object{
                    {"controllable_component_id", CanonicalValue("device")},
                    {"signal_name", CanonicalValue("blocked")}})}})},
                {"op", CanonicalValue("==")}, {"value", CanonicalValue(true)}})}})},
        {"scope", CanonicalValue("all_motion")}};
    for (auto& field : extra) fields.push_back(std::move(field));
    auto policies = interrupt_policy_envelope("", "");
    policies.pop_back();
    policies += ",\"interrupt_rules\":[" + canonical_json(CanonicalValue(std::move(fields))) + "]}";
    return with_policies(minimal_document(), std::move(policies));
  };
  const auto sealed = [&](CanonicalValue::Object extra) {
    const auto decoded = decode(document(std::move(extra)));
    return decoded.normalized_policies()->find_object_member("interrupt_rules")
        ->as_array().front();
  };
  const auto gate = sealed({});
  ASSERT_NE(gate.find_object_member("on_trip"), nullptr);
  EXPECT_EQ(gate.find_object_member("on_trip")->as_string(), "gate");
  EXPECT_EQ(gate.find_object_member("idle_behaviour"), nullptr);
  EXPECT_EQ(sealed({{"on_trip", CanonicalValue("gate")}}), gate);
  const auto full = sealed({{"on_trip", CanonicalValue("full_stop")},
                            {"idle_behaviour", CanonicalValue("latch")}});
  EXPECT_EQ(full.find_object_member("on_trip")->as_string(), "full_stop");
  EXPECT_EQ(full.find_object_member("idle_behaviour")->as_string(), "latch");
  const auto notify = sealed({{"on_trip", CanonicalValue("notify")}});
  ASSERT_NE(notify.find_object_member("on_trip"), nullptr);
  EXPECT_EQ(notify.find_object_member("on_trip")->as_string(), "notify");
  EXPECT_EQ(notify.find_object_member("idle_behaviour"), nullptr);
  EXPECT_EQ(notify.find_object_member("stop_entry_stage_id"), nullptr);
  for (const auto *mode : {"gate", "full_stop"}) {
    const auto with_entry = sealed({{"on_trip", CanonicalValue(mode)},
                                    {"stop_entry_stage_id", CanonicalValue("stage")}});
    ASSERT_NE(with_entry.find_object_member("stop_entry_stage_id"), nullptr);
    EXPECT_EQ(with_entry.find_object_member("stop_entry_stage_id")->as_string(), "stage");
  }
  for (auto invalid : std::vector<CanonicalValue::Object>{
           {{"on_trip", CanonicalValue("stop")}},
           {{"on_trip", CanonicalValue(true)}},
           {{"on_trip", CanonicalValue(nullptr)}},
           {{"idle_behaviour", CanonicalValue("latch")}},
           {{"on_trip", CanonicalValue("gate")}, {"idle_behaviour", CanonicalValue("ignore")}},
           {{"on_trip", CanonicalValue("notify")}, {"idle_behaviour", CanonicalValue("latch")}},
           {{"on_trip", CanonicalValue("notify")}, {"idle_behaviour", CanonicalValue("ignore")}},
           {{"on_trip", CanonicalValue("notify")}, {"stop_entry_stage_id", CanonicalValue("stage")}},
           {{"latch_release", CanonicalValue("operator")}}})
    EXPECT_THROW(static_cast<void>(decode(document(std::move(invalid)))),
                 DeploymentApplicabilityCodecError);
}

} // namespace manyforge::deployment
