#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <manyforge_deployment/artifact_codec.hpp>
#include <manyforge_deployment/canonical_value.hpp>
#include <manyforge_deployment/content_identity.hpp>
#include <manyforge_deployment/deployment_artifact_codec.hpp>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace manyforge::deployment {
namespace {

constexpr std::string_view kZeroDigest =
    "sha256:0000000000000000000000000000000000000000000000000000000000000000";
constexpr std::string_view kOneDigest =
    "sha256:1111111111111111111111111111111111111111111111111111111111111111";

static_assert(!std::is_aggregate_v<RevisionVerifiedDeploymentArtifact>);
static_assert(
    !std::is_default_constructible_v<RevisionVerifiedDeploymentArtifact>);
static_assert(!std::is_constructible_v<
              RevisionVerifiedDeploymentArtifact,
              core::DeploymentArtifactId, core::DeploymentRevision,
              core::CellId, std::string, std::optional<std::string>,
              std::vector<core::CatalogRecordRefDTO>,
              GraphRevisionVerifiedCellGraph,
              RevisionVerifiedDeploymentApplicabilityInputs, bool,
              CanonicalValue>);

[[nodiscard]] std::string empty_graph() {
  return "{\"runtime_units\":[],\"model_instances\":[],"
         "\"controllable_components\":[],\"control_domains\":[],"
         "\"control_bindings\":[],\"component_attachments\":[]}";
}

[[nodiscard]] std::string control_domain_graph(
    std::string_view domain_id = "arm_domain") {
  return "{\"runtime_units\":[],\"model_instances\":[],"
         "\"controllable_components\":[],\"control_domains\":[{"
         "\"control_domain_id\":\"" +
         std::string(domain_id) +
         "\",\"control_domain_revision\":\"" + std::string(kZeroDigest) +
         "\",\"runtime_unit_ids\":[],"
         "\"control_coordinator_endpoint_role\":\"coordinator\","
         "\"controller_topology_id\":\"topology\","
         "\"topology_revision\":\"" + std::string(kOneDigest) +
         "\",\"containment_granularity\":\"control_domain\"}],"
         "\"control_bindings\":[],\"component_attachments\":[]}";
}

[[nodiscard]] std::string provider_binding() {
  return "{\"capability_provider_binding_id\":\"provider_binding\","
         "\"capability_provider_binding_revision\":\"" +
         std::string(kZeroDigest) +
         "\",\"capability_id\":\"planning.motion\","
         "\"capability_definition_revision\":\"" +
         std::string(kOneDigest) +
         "\",\"scope\":\"cell_global\","
         "\"target\":{\"cell_id\":\"cell_a\"},"
         "\"provider_instance_id\":\"provider\","
         "\"provider_manifest_id\":\"provider_manifest\","
         "\"provider_manifest_revision\":\"" +
         std::string(kOneDigest) + "\",\"endpoint_bindings\":[]}";
}

[[nodiscard]] std::string document(
    std::string graph = empty_graph(), std::string providers = "[]",
    std::string catalogs = "{\"records\":[]}",
    std::string optional_members = "",
    std::string_view artifact_id = "deployment_a",
    std::string_view cell_id = "cell_a", std::string_view name = "Cell A") {
  return "{\"api_version\":\"manyforge.deployment.v1\","
         "\"deployment_artifact_id\":\"" +
         std::string(artifact_id) + "\",\"deployment_revision\":\"" +
         std::string(kZeroDigest) + "\",\"cell_id\":\"" +
         std::string(cell_id) + "\",\"name\":\"" + std::string(name) +
         "\",\"catalogs\":" + std::move(catalogs) +
         ",\"graph\":" + std::move(graph) +
         ",\"desired_control_configurations\":[],"
         "\"capability_provider_bindings\":" +
         std::move(providers) + std::move(optional_members) + "}";
}

[[nodiscard]] std::string aggregate_expanding_yaml_document() {
  constexpr std::size_t kCatalogRecordCount = 8U;
  constexpr std::size_t kStoreKeyBytes = 930000U;
  constexpr std::size_t kDescriptionBytes = 950000U;

  std::string authored;
  authored.reserve(kCatalogRecordCount * kStoreKeyBytes +
                   kDescriptionBytes + 8192U);
  authored +=
      "api_version: manyforge.deployment.v1\n"
      "deployment_artifact_id: deployment_a\n"
      "deployment_revision: ";
  authored += kZeroDigest;
  authored +=
      "\ncell_id: cell_a\n"
      "name: Cell A\n"
      "description: '";
  authored.append(kDescriptionBytes, '\\');
  authored +=
      "'\n"
      "catalogs:\n"
      "  records:\n";

  for (std::size_t index = 0U; index < kCatalogRecordCount; ++index) {
    authored +=
        "    - record_kind: model_asset\n"
        "      record_id: model_";
    authored += std::to_string(index);
    authored +=
        "\n"
        "      record_revision: ";
    authored += kZeroDigest;
    authored +=
        "\n"
        "      schema_id: manyforge.model_asset.v1\n"
        "      schema_revision: ";
    authored += kZeroDigest;
    authored +=
        "\n"
        "      content_digest: ";
    authored += kZeroDigest;
    authored +=
        "\n"
        "      content_addressed_store_key: '";
    authored.append(kStoreKeyBytes, '\\');
    authored += "'\n";
  }

  authored +=
      "graph:\n"
      "  runtime_units: []\n"
      "  model_instances: []\n"
      "  controllable_components: []\n"
      "  control_domains: []\n"
      "  control_bindings: []\n"
      "  component_attachments: []\n"
      "desired_control_configurations: []\n"
      "capability_provider_bindings: []\n";
  return authored;
}

[[nodiscard]] RevisionVerifiedDeploymentArtifact produce(
    std::string_view authored) {
  return compute_deployment_artifact_revision(
      parse_strict_json_model_yaml(authored),
      std::span<const QualifiedRuntimeProfile>{});
}

[[nodiscard]] RevisionVerifiedDeploymentArtifact verify(
    const CanonicalValue& authored) {
  return verify_deployment_artifact_revision(
      parse_strict_json_model_yaml(canonical_json(authored)),
      std::span<const QualifiedRuntimeProfile>{});
}

[[nodiscard]] CanonicalValue replace_member(
    const CanonicalValue& object_value, std::string_view member_name,
    CanonicalValue replacement) {
  CanonicalValue::Object object = object_value.as_object();
  const auto iterator =
      std::find_if(object.begin(), object.end(),
                   [member_name](const auto& member) {
                     return member.first == member_name;
                   });
  EXPECT_NE(iterator, object.end());
  if (iterator != object.end()) {
    iterator->second = std::move(replacement);
  }
  return CanonicalValue(std::move(object));
}

[[nodiscard]] CanonicalValue remove_member(
    const CanonicalValue& object_value, std::string_view member_name) {
  CanonicalValue::Object object = object_value.as_object();
  const auto iterator =
      std::find_if(object.begin(), object.end(),
                   [member_name](const auto& member) {
                     return member.first == member_name;
                   });
  EXPECT_NE(iterator, object.end());
  if (iterator != object.end()) {
    object.erase(iterator);
  }
  return CanonicalValue(std::move(object));
}

[[nodiscard]] CanonicalValue mutate_first_array_record(
    const CanonicalValue& root, std::string_view table_name,
    std::string_view revision_name, std::string replacement) {
  const CanonicalValue* table = root.find_object_member(table_name);
  EXPECT_NE(table, nullptr);
  CanonicalValue::Array rows =
      std::get<CanonicalValue::Array>(table->storage());
  EXPECT_FALSE(rows.empty());
  if (!rows.empty()) {
    rows.front() =
        replace_member(rows.front(), revision_name,
                       CanonicalValue(std::move(replacement)));
  }
  return replace_member(root, table_name,
                        CanonicalValue(std::move(rows)));
}

[[nodiscard]] std::string independent_deployment_identity(
    const CanonicalValue& normalized_root) {
  CanonicalValue::Object payload;
  bool found_own_field = false;
  for (const auto& [name, value] : normalized_root.as_object()) {
    if (name == "deployment_revision") {
      found_own_field = true;
      continue;
    }
    payload.emplace_back(name, value);
  }
  EXPECT_TRUE(found_own_field);

  std::string preimage{"manyforge.content-identity.v1"};
  preimage.push_back('\0');
  preimage.append("deployment-revision");
  preimage.push_back('\0');
  preimage.append("manyforge.deployment.v1");
  preimage.push_back('\0');
  preimage.append(canonical_json(CanonicalValue(std::move(payload))));
  return sha256_digest(preimage);
}

struct CatalogFixture final {
  std::string_view kind;
  std::string_view id;
  std::string_view family;
};

constexpr std::array<CatalogFixture, 11> kAllCatalogKinds{{
    {"model_asset", "model", "manyforge.model_asset.v1"},
    {"runtime_profile", "runtime", "manyforge.runtime_profile.v1"},
    {"component_type", "component", "manyforge.component_type.v1"},
    {"control_interface_contract", "contract",
     "manyforge.control_interface_contract.v1"},
    {"controller_topology", "topology",
     "manyforge.controller_topology.v1"},
    {"provider_manifest", "provider",
     "manyforge.provider_manifest.v1"},
    {"command_schema", "command", "manyforge.command_schema.v1"},
    {"capability_definition", "capability",
     "manyforge.capability_definition.v1"},
    {"contact_policy", "contact", "manyforge.contact_policy.v1"},
    {"stop_evidence_policy", "stop",
     "manyforge.stop_evidence_policy.v1"},
    {"runtime_policy", "runtime_policy",
     "manyforge.runtime_policy.v1"},
}};

void expect_compute_error(std::string authored,
                          std::string_view expected_fragment) {
  try {
    (void)produce(authored);
    FAIL() << "expected DeploymentArtifactCodecError";
  } catch (const DeploymentArtifactCodecError& error) {
    EXPECT_NE(std::string_view(error.what()).find(expected_fragment),
              std::string_view::npos)
        << error.what();
  }
}

void expect_verify_error(const CanonicalValue& authored,
                         std::string_view expected_fragment) {
  try {
    (void)verify(authored);
    FAIL() << "expected DeploymentArtifactCodecError";
  } catch (const DeploymentArtifactCodecError& error) {
    EXPECT_NE(std::string_view(error.what()).find(expected_fragment),
              std::string_view::npos)
        << error.what();
  }
}

TEST(DeploymentArtifactCodec,
     CanonicalEmptyProducerAndVerifierPreserveOneExactIdentityRoot) {
  const auto produced = produce(document());
  EXPECT_EQ(produced.deployment_artifact_id().value(), "deployment_a");
  EXPECT_EQ(produced.cell_id().value(), "cell_a");
  EXPECT_EQ(produced.name(), "Cell A");
  EXPECT_FALSE(produced.description().has_value());
  EXPECT_TRUE(produced.catalog_records().empty());
  EXPECT_FALSE(produced.assistant_was_present());
  EXPECT_EQ(produced.graph().graph().cell_id.value(), "cell_a");
  EXPECT_TRUE(produced.graph().graph().runtime_units.empty());
  EXPECT_EQ(produced.deployment_revision().value(),
            independent_deployment_identity(
                produced.normalized_authored_root()));

  const auto verified = verify(produced.normalized_authored_root());
  EXPECT_EQ(verified.deployment_revision(),
            produced.deployment_revision());
  EXPECT_EQ(verified.graph().graph().graph_revision,
            produced.graph().graph().graph_revision);
  EXPECT_EQ(verified.normalized_authored_root(),
            produced.normalized_authored_root());
  EXPECT_EQ(verified.normalized_authored_graph(),
            produced.normalized_authored_graph());

  const auto source_copy = produced;
  const auto destination = source_copy;
  EXPECT_EQ(&source_copy.normalized_authored_root(),
            &destination.normalized_authored_root());
  EXPECT_EQ(&source_copy.graph(), &destination.graph());
  EXPECT_EQ(&source_copy.applicability_inputs(),
            &destination.applicability_inputs());
  EXPECT_EQ(&source_copy.catalog_records(), &destination.catalog_records());
  EXPECT_EQ(source_copy.normalized_authored_root(),
            destination.normalized_authored_root());
  EXPECT_EQ(verify(source_copy.normalized_authored_root())
                .deployment_revision(),
            source_copy.deployment_revision());
}

TEST(DeploymentArtifactCodec,
     IdentityUsesTheDomainSeparatedExactDeploymentPreimage) {
  const auto produced =
      produce(document(empty_graph(), "[]", "{\"records\":[]}",
                       ",\"description\":\"Description\","
                       "\"assistant\":{}"));
  EXPECT_EQ(produced.deployment_revision().value(),
            independent_deployment_identity(
                produced.normalized_authored_root()));
  EXPECT_EQ(produced.deployment_revision().value(),
            "sha256:910539f01d02d63e5a0da96c289b2e5cad39b255ea62a57d8e9f67eb81cb80c9");
}

TEST(DeploymentArtifactCodec,
     DetachedProofOwnsTheCompleteParsedDeploymentAndNestedProofs) {
  const auto detached = [] {
    std::string authored = document(
        empty_graph(), "[]", "{\"records\":[]}",
        ",\"description\":\"Detached\",\"external_resources\":[],"
        "\"assistant\":{}");
    const auto parsed = parse_strict_json_model_yaml(authored);
    return compute_deployment_artifact_revision(
        parsed, std::span<const QualifiedRuntimeProfile>{});
  }();
  const auto copied = detached;

  EXPECT_EQ(detached.description(), std::optional<std::string>("Detached"));
  EXPECT_TRUE(detached.assistant_was_present());
  EXPECT_TRUE(
      detached.applicability_inputs().external_resources_was_present());
  EXPECT_EQ(&detached.normalized_authored_root(),
            &copied.normalized_authored_root());
  EXPECT_EQ(&detached.graph(), &copied.graph());
  EXPECT_EQ(verify(detached.normalized_authored_root()).deployment_revision(),
            detached.deployment_revision());
}

TEST(DeploymentArtifactCodec,
     PreservesEveryQualifiedOptionalPresenceState) {
  const std::array<std::string, 9> optional_members{{
      "",
      ",\"description\":\"Description\"",
      ",\"site_overlay_ref\":null",
      ",\"site_overlay_ref\":{\"site_overlay_id\":\"plant\","
      "\"site_overlay_revision\":\"" +
          std::string(kZeroDigest) + "\",\"non_secret_digest\":\"" +
          std::string(kOneDigest) + "\"}",
      ",\"external_resources\":[]",
      ",\"assistant\":{}",
      ",\"site_overlay_ref\":null,\"external_resources\":[],"
      "\"assistant\":{}",
      ",\"description\":\"Description\",\"site_overlay_ref\":null,"
      "\"external_resources\":[],\"assistant\":{}",
      ",\"site_overlay_ref\":{\"site_overlay_id\":\"plant\","
      "\"site_overlay_revision\":\"" +
          std::string(kZeroDigest) + "\",\"non_secret_digest\":\"" +
          std::string(kOneDigest) +
          "\"},\"external_resources\":[{"
          "\"external_resource_alias\":\"endpoint\","
          "\"resource_kind\":\"service_endpoint\","
          "\"provider_instance_id\":\"provider\","
          "\"provider_manifest_id\":\"provider_manifest\","
          "\"provider_manifest_revision\":\"" +
          std::string(kOneDigest) +
          "\",\"endpoint_role_id\":\"service\","
          "\"site_overlay_value_ref\":\"resources.endpoint\"}]",
  }};

  std::set<std::string> revisions;
  std::optional<core::DeploymentRevision> baseline_revision;
  for (const auto& optional : optional_members) {
    SCOPED_TRACE(optional);
    const auto authored_document =
        document(empty_graph(), "[]", "{\"records\":[]}", optional);
    const auto authored =
        parse_strict_json_model_yaml(authored_document);
    const auto produced =
        produce(authored_document);
    const auto verified = verify(produced.normalized_authored_root());
    EXPECT_EQ(verified.normalized_authored_root(),
              produced.normalized_authored_root());
    EXPECT_EQ(verified.assistant_was_present(),
              optional.find("\"assistant\"") != std::string::npos);
    EXPECT_EQ(
        verified.applicability_inputs().external_resources_was_present(),
        optional.find("\"external_resources\"") != std::string::npos);

    for (const std::string_view member :
         {"description", "site_overlay_ref", "external_resources",
          "assistant"}) {
      const CanonicalValue* expected =
          authored.root().find_object_member(member);
      const CanonicalValue* actual =
          produced.normalized_authored_root().find_object_member(member);
      ASSERT_EQ(actual == nullptr, expected == nullptr) << member;
      if (expected != nullptr) {
        EXPECT_EQ(*actual, *expected) << member;
      }
    }

    EXPECT_TRUE(
        revisions.insert(produced.deployment_revision().value()).second);
    if (!baseline_revision.has_value()) {
      baseline_revision = produced.deployment_revision();
    } else {
      CanonicalValue stale = replace_member(
          produced.normalized_authored_root(), "deployment_revision",
          CanonicalValue(baseline_revision->value()));
      expect_verify_error(stale, "deployment_revision does not match");
    }
  }
  EXPECT_EQ(revisions.size(), optional_members.size());
}

TEST(DeploymentArtifactCodec,
     ClosesTopLevelGraphAndOptionalBlockShapesBeforeIdentity) {
  const CanonicalValue missing_name = remove_member(
      parse_strict_json_model_yaml(document()).root(), "name");
  expect_compute_error(canonical_json(missing_name),
                       "missing required field 'name'");
  expect_compute_error(
      document(empty_graph(), "[]", "{\"records\":[]}",
               ",\"unknown\":{}"),
      "unknown field 'unknown'");
  expect_compute_error(
      document("{\"runtime_units\":[],\"model_instances\":[],"
               "\"controllable_components\":[],\"control_domains\":[],"
               "\"control_bindings\":[]}"),
      "missing required field 'component_attachments'");
  expect_compute_error(
      document("{\"runtime_units\":[],\"model_instances\":[],"
               "\"controllable_components\":[],\"control_domains\":[],"
               "\"control_bindings\":[],\"component_attachments\":[],"
               "\"extra\":[]}"),
      "unknown field 'extra'");
  expect_compute_error(
      document(empty_graph(), "[]", "{\"records\":[]}",
               ",\"description\":null"),
      "$.description: expected string");
  expect_compute_error(
      document(empty_graph(), "[]", "{\"records\":[]}",
               ",\"assistant\":[]"),
      "$.assistant: expected object");
  expect_compute_error(
      document(empty_graph(), "[]", "{\"records\":[]}",
               ",\"assistant\":{\"enabled\":false}"),
      "only the exact empty assistant object");
  expect_compute_error(
      document(empty_graph(), "[]", "{\"records\":[]}",
               ",\"policies\":{}"),
      "policy block is not qualified");
  expect_compute_error(
      document(empty_graph(), "[]", "{\"records\":[]}",
               ",\"compatibility\":{}"),
      "compatibility admission is not qualified");
}

TEST(DeploymentArtifactCodec,
     ProducerMintsAndVerifierIndependentlyChecksNestedGraphRevisions) {
  const auto produced = produce(document(control_domain_graph()));
  ASSERT_EQ(produced.graph().graph().control_domains.size(), 1U);
  EXPECT_NE(
      produced.graph().graph().control_domains.front()
          .control_domain_revision.value(),
      kZeroDigest);

  const CanonicalValue* graph =
      produced.normalized_authored_root().find_object_member("graph");
  ASSERT_NE(graph, nullptr);
  CanonicalValue stale_graph = mutate_first_array_record(
      *graph, "control_domains", "control_domain_revision",
      std::string(kZeroDigest));
  CanonicalValue stale_root =
      replace_member(produced.normalized_authored_root(), "graph",
                     std::move(stale_graph));
  expect_verify_error(stale_root, "graph-owned nested revision");
}

TEST(DeploymentArtifactCodec,
     ProducerMintsAndVerifierIndependentlyChecksProviderBindingRevisions) {
  const auto produced =
      produce(document(
          empty_graph(),
          std::string("[") + provider_binding() + "]"));
  ASSERT_EQ(produced.applicability_inputs()
                .value()
                .capability_provider_bindings.size(),
            1U);
  EXPECT_NE(
      produced.applicability_inputs()
          .value()
          .capability_provider_bindings.front()
          .capability_provider_binding_revision.value(),
      kZeroDigest);

  CanonicalValue stale_root = mutate_first_array_record(
      produced.normalized_authored_root(),
      "capability_provider_bindings",
      "capability_provider_binding_revision", std::string(kZeroDigest));
  expect_verify_error(stale_root,
                      "capability_provider_binding_revision does not match");
}

TEST(DeploymentArtifactCodec,
     VerifierRejectsStaleDeploymentRevisionAndCrossRootSplices) {
  const auto first = produce(document(control_domain_graph("domain_a")));
  const auto second = produce(document(control_domain_graph("domain_b"),
                                       "[]", "{\"records\":[]}", "",
                                       "deployment_b", "cell_b", "Cell B"));

  CanonicalValue renamed =
      replace_member(first.normalized_authored_root(), "name",
                     CanonicalValue("Changed"));
  expect_verify_error(renamed, "deployment_revision does not match");

  const CanonicalValue* second_graph =
      second.normalized_authored_root().find_object_member("graph");
  ASSERT_NE(second_graph, nullptr);
  CanonicalValue cross_splice =
      replace_member(first.normalized_authored_root(), "graph",
                     *second_graph);
  expect_verify_error(cross_splice,
                      "deployment_revision does not match");
}

TEST(DeploymentArtifactCodec,
     RetainsTheEightCurrentlyQualifiedCatalogReferenceVariants) {
  constexpr std::array<std::size_t, 8> kQualifiedIndices{
      0U, 1U, 2U, 3U, 4U, 5U, 7U, 9U};
  std::string records;
  for (const std::size_t index : kQualifiedIndices) {
    const auto& row = kAllCatalogKinds[index];
    if (!records.empty()) {
      records.push_back(',');
    }
    const char digit = "123456789ab"[index];
    records +=
        "{\"record_kind\":\"" + std::string(row.kind) +
        "\",\"record_id\":\"" + std::string(row.id) +
        "\",\"record_revision\":\"sha256:" + std::string(64U, digit) +
        "\",\"schema_id\":\"" + std::string(row.family) +
        "\",\"schema_revision\":\"sha256:" +
        std::string(64U, digit) +
        "\",\"content_digest\":\"sha256:" + std::string(64U, digit) +
        "\",\"content_addressed_store_key\":\"catalog/" +
        std::string(row.id) + "\"}";
  }
  const auto produced = produce(document(
      empty_graph(), "[]", "{\"records\":[" + records + "]}"));
  ASSERT_EQ(produced.catalog_records().size(), kQualifiedIndices.size());
  for (const std::size_t index : kQualifiedIndices) {
    const auto& expected = kAllCatalogKinds[index];
    const auto found = std::find_if(
        produced.catalog_records().begin(),
        produced.catalog_records().end(),
        [&expected](const core::CatalogRecordRefDTO& actual) {
          return core::to_string(core::catalog_record_kind(actual)) ==
                     expected.kind &&
                 core::catalog_record_id_value(actual) == expected.id;
        });
    EXPECT_NE(found, produced.catalog_records().end());
  }
  EXPECT_EQ(verify(produced.normalized_authored_root()).catalog_records(),
            produced.catalog_records());
}

TEST(DeploymentArtifactCodec,
     RejectsTheThreeCatalogFamiliesOwnedByLaterSlices) {
  constexpr std::array<std::size_t, 3> kDeferredIndices{6U, 8U, 10U};
  for (const std::size_t index : kDeferredIndices) {
    const auto& row = kAllCatalogKinds[index];
    const char digit = "123456789ab"[index];
    const std::string catalogs =
        "{\"records\":[{\"record_kind\":\"" +
        std::string(row.kind) +
        "\",\"record_id\":\"" + std::string(row.id) +
        "\",\"record_revision\":\"sha256:" + std::string(64U, digit) +
        "\",\"schema_id\":\"" + std::string(row.family) +
        "\",\"schema_revision\":\"sha256:" +
        std::string(64U, digit) +
        "\",\"content_digest\":\"sha256:" + std::string(64U, digit) +
        "\",\"content_addressed_store_key\":\"catalog/" +
        std::string(row.id) + "\"}]}";
    expect_compute_error(
        document(empty_graph(), "[]", catalogs),
        "is not qualified by the current compiler subset");
  }
}

TEST(DeploymentArtifactCodec,
     RejectsAggregateNormalizedEnvelopeExpansionBeforeIdentityOnBothPaths) {
  const std::string authored = aggregate_expanding_yaml_document();
  ASSERT_LT(authored.size(), kMaxArtifactEncodedBytes);
  const auto expect_envelope_budget_rejection = [](auto&& operation) {
    try {
      (void)operation();
      FAIL() << "expected DeploymentArtifactCodecError";
    } catch (const DeploymentArtifactCodecError& error) {
      const std::string_view message(error.what());
      EXPECT_NE(message.find("deployment envelope: canonical_value.members["),
                std::string_view::npos)
          << message;
      EXPECT_NE(message.find("normalized scalar bytes exceed 16777216"),
                std::string_view::npos)
          << message;
      EXPECT_EQ(message.find("deployment_revision does not match"),
                std::string_view::npos)
          << message;
    }
  };

  expect_envelope_budget_rejection([&] { return produce(authored); });
  expect_envelope_budget_rejection([&] {
    return verify_deployment_artifact_revision(
        parse_strict_json_model_yaml(authored),
        std::span<const QualifiedRuntimeProfile>{});
  });
}

}  // namespace
}  // namespace manyforge::deployment
