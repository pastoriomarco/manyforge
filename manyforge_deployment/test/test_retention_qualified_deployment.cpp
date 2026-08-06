#include <gtest/gtest.h>

#include <manyforge_deployment/artifact_codec.hpp>
#include <manyforge_deployment/cell_applicability.hpp>
#include <manyforge_deployment/cell_compiler.hpp>
#include <manyforge_deployment/cell_composition.hpp>
#include <manyforge_deployment/deployment_artifact_codec.hpp>
#include <manyforge_deployment/retention_qualified_deployment.hpp>
#include <manyforge_deployment/site_overlay_resolution.hpp>

#include <span>
#include <string>
#include <string_view>
#include <type_traits>

#include "runtime_only_retention_fixture.hpp"

namespace manyforge::deployment {
namespace {

constexpr std::string_view kZeroDigest =
    "sha256:0000000000000000000000000000000000000000000000000000000000000000";
constexpr std::string_view kOneDigest =
    "sha256:1111111111111111111111111111111111111111111111111111111111111111";

static_assert(!std::is_aggregate_v<RetentionQualifiedDeployment>);
static_assert(!std::is_default_constructible_v<RetentionQualifiedDeployment>);
static_assert(
    !std::is_constructible_v<RetentionQualifiedDeployment,
                             RevisionVerifiedDeploymentArtifact,
                             CompilerQualifiedCellIr>);

[[nodiscard]] std::string empty_graph() {
  return "{\"runtime_units\":[],\"model_instances\":[],"
         "\"controllable_components\":[],\"control_domains\":[],"
         "\"control_bindings\":[],\"component_attachments\":[]}";
}

[[nodiscard]] std::string one_domain_graph() {
  return "{\"runtime_units\":[],\"model_instances\":[],"
         "\"controllable_components\":[],\"control_domains\":[{"
         "\"control_domain_id\":\"domain\","
         "\"control_domain_revision\":\"" +
         std::string(kZeroDigest) +
         "\",\"runtime_unit_ids\":[],"
         "\"control_coordinator_endpoint_role\":\"coordinator\","
         "\"controller_topology_id\":\"topology\","
         "\"topology_revision\":\"" +
         std::string(kOneDigest) +
         "\",\"containment_granularity\":\"control_domain\"}],"
         "\"control_bindings\":[],\"component_attachments\":[]}";
}

[[nodiscard]] std::string desired_configuration() {
  return "{\"control_domain_id\":\"domain\","
         "\"control_domain_revision\":\"" +
         std::string(kZeroDigest) +
         "\",\"controller_topology_id\":\"topology\","
         "\"topology_revision\":\"" +
         std::string(kOneDigest) +
         "\",\"activation_set_id\":\"active\","
         "\"binding_routes\":[{"
         "\"control_binding_id\":\"binding\","
         "\"control_binding_revision\":\"" +
         std::string(kOneDigest) +
         "\",\"control_route_id\":\"route\"}]}";
}

[[nodiscard]] std::string provider_binding() {
  return "{\"capability_provider_binding_id\":\"provider_binding\","
         "\"capability_provider_binding_revision\":\"" +
         std::string(kZeroDigest) +
         "\",\"capability_id\":\"planning.motion\","
         "\"capability_definition_revision\":\"" +
         std::string(kOneDigest) +
         "\",\"scope\":\"cell_global\","
         "\"target\":{\"cell_id\":\"cell\"},"
         "\"provider_instance_id\":\"provider\","
         "\"provider_manifest_id\":\"manifest\","
         "\"provider_manifest_revision\":\"" +
         std::string(kOneDigest) +
         "\",\"endpoint_bindings\":[]}";
}

[[nodiscard]] std::string external_resource() {
  return "{\"external_resource_alias\":\"endpoint\","
         "\"resource_kind\":\"service_endpoint\","
         "\"provider_instance_id\":\"provider\","
         "\"provider_manifest_id\":\"manifest\","
         "\"provider_manifest_revision\":\"" +
         std::string(kOneDigest) +
         "\",\"endpoint_role_id\":\"service\","
         "\"site_overlay_value_ref\":\"resources.endpoint\"}";
}

[[nodiscard]] std::string one_catalog_record() {
  return "{\"records\":[{"
         "\"record_kind\":\"model_asset\","
         "\"record_id\":\"model\","
         "\"record_revision\":\"" +
         std::string(kOneDigest) +
         "\",\"schema_id\":\"manyforge.model_asset.v1\","
         "\"schema_revision\":\"" +
         std::string(kOneDigest) +
         "\",\"content_digest\":\"" +
         std::string(kOneDigest) +
         "\",\"content_addressed_store_key\":\"catalog/model/1\"}]}";
}

[[nodiscard]] std::string deployment_document(
    std::string_view cell_id = "cell",
    std::string graph = empty_graph(),
    std::string desired = "[]",
    std::string providers = "[]",
    std::string catalogs = "{\"records\":[]}",
    std::string optional = "") {
  return "{\"api_version\":\"manyforge.deployment.v1\","
         "\"deployment_artifact_id\":\"deployment\","
         "\"deployment_revision\":\"" +
         std::string(kZeroDigest) +
         "\",\"cell_id\":\"" + std::string(cell_id) +
         "\",\"name\":\"Retention fixture\","
         "\"catalogs\":" + std::move(catalogs) +
         ",\"graph\":" + std::move(graph) +
         ",\"desired_control_configurations\":" + std::move(desired) +
         ",\"capability_provider_bindings\":" + std::move(providers) +
         std::move(optional) + "}";
}

[[nodiscard]] RevisionVerifiedDeploymentArtifact produce(
    std::string document) {
  return compute_deployment_artifact_revision(
      parse_strict_json_model_yaml(document),
      std::span<const QualifiedRuntimeProfile>{});
}

[[nodiscard]] CompilerQualifiedCellIr compile_empty_from(
    const RevisionVerifiedDeploymentArtifact& deployment) {
  const auto composition = compose_cell_occurrences(
      deployment.graph(),
      std::span<const SourceVerifiedModelAsset>{},
      std::span<const QualifiedComponentType>{});
  const auto selected_overlay = select_site_overlay_for_deployment(
      deployment.applicability_inputs().value().site_overlay_ref,
      deployment.applicability_inputs().value().external_resources,
      nullptr);
  const auto applicability = qualify_cell_applicability(
      composition, deployment.applicability_inputs(), {}, {}, {}, {}, {},
      {}, {}, selected_overlay);
  return compile_cell_ir(applicability);
}

void expect_mismatch(const RevisionVerifiedDeploymentArtifact& deployment,
                     const CompilerQualifiedCellIr& compiler,
                     std::string_view projection) {
  try {
    (void)qualify_deployment_for_retention(deployment, compiler);
    FAIL() << "expected RetentionQualificationError";
  } catch (const RetentionQualificationError& error) {
    EXPECT_NE(std::string_view(error.what()).find(projection),
              std::string_view::npos)
        << error.what();
  }
}

TEST(RetentionQualifiedDeployment,
     CanonicalEmptyMatchesAndCopiesShareImmutableProofs) {
  const auto deployment = produce(deployment_document(
      "cell", empty_graph(), "[]", "[]", "{\"records\":[]}",
      ",\"external_resources\":[],\"assistant\":{}"));
  const auto compiler = compile_empty_from(deployment);
  const auto qualified =
      qualify_deployment_for_retention(deployment, compiler);
  const auto copied = qualified;

  EXPECT_EQ(&qualified.deployment(), &copied.deployment());
  EXPECT_EQ(&qualified.compiler_ir(), &copied.compiler_ir());
  EXPECT_EQ(qualified.deployment().deployment_revision(),
            deployment.deployment_revision());
  EXPECT_EQ(qualified.compiler_ir().applicability().composition().graph(),
            deployment.graph().graph());
}

TEST(RetentionQualifiedDeployment,
     RejectsCellGraphAndCatalogCrossRootSplicesIndependently) {
  const auto baseline_deployment = produce(deployment_document());
  const auto baseline_compiler = compile_empty_from(baseline_deployment);

  expect_mismatch(
      produce(deployment_document("other_cell")),
      baseline_compiler, "cell_id");
  expect_mismatch(
      produce(deployment_document("cell", one_domain_graph())),
      baseline_compiler, "complete cell_graph");
  expect_mismatch(
      produce(deployment_document(
          "cell", empty_graph(), "[]", "[]", one_catalog_record())),
      baseline_compiler, "catalog/source reference union");
}

TEST(RetentionQualifiedDeployment,
     RejectsEveryApplicabilityAndPresenceProjectionBeforeRetention) {
  const auto baseline_deployment = produce(deployment_document());
  const auto baseline_compiler = compile_empty_from(baseline_deployment);

  expect_mismatch(
      produce(deployment_document(
          "cell", empty_graph(),
          std::string("[") + desired_configuration() + "]")),
      baseline_compiler, "applicability value tables");
  expect_mismatch(
      produce(deployment_document(
          "cell", empty_graph(), "[]",
          std::string("[") + provider_binding() + "]")),
      baseline_compiler, "applicability value tables");
  expect_mismatch(
      produce(deployment_document(
          "cell", empty_graph(), "[]", "[]", "{\"records\":[]}",
          ",\"site_overlay_ref\":{\"site_overlay_id\":\"plant\","
          "\"site_overlay_revision\":\"" +
              std::string(kOneDigest) +
              "\",\"non_secret_digest\":\"" +
              std::string(kZeroDigest) +
              "\"},\"external_resources\":[" +
              external_resource() + "]")),
      baseline_compiler, "applicability value tables");

  // These two pairs have the same semantic empty value tables. Their source
  // spelling is nevertheless part of DeploymentRevision and must not splice.
  expect_mismatch(
      produce(deployment_document(
          "cell", empty_graph(), "[]", "[]", "{\"records\":[]}",
          ",\"external_resources\":[]")),
      baseline_compiler, "external_resources source presence");
  expect_mismatch(
      produce(deployment_document(
          "cell", empty_graph(), "[]", "[]", "{\"records\":[]}",
          ",\"site_overlay_ref\":null")),
      baseline_compiler, "site_overlay_ref source presence");
}

TEST(RetentionQualifiedDeployment,
     NonEmptyRuntimeEnvelopeAndCompilerMatchThroughTheFinalTrustBoundary) {
  const auto fixture = test::make_runtime_only_retention_fixture();

  ASSERT_EQ(fixture.deployment.graph().graph().runtime_units.size(), 1U);
  ASSERT_EQ(fixture.compiler.applicability().runtime_profiles().size(), 1U);
  ASSERT_EQ(fixture.compiler.catalog_record_refs().size(), 1U);
  EXPECT_EQ(fixture.qualified.deployment().deployment_revision(),
            fixture.deployment.deployment_revision());
  EXPECT_EQ(fixture.qualified.compiler_ir().catalog_record_refs(),
            fixture.deployment.catalog_records());

  // The returned proof owns the complete non-empty predecessors after every
  // helper-local candidate, profile document, and composition object is gone.
  const auto detached = [] {
    return test::make_runtime_only_retention_fixture().qualified;
  }();
  ASSERT_EQ(detached.deployment().graph().graph().runtime_units.size(), 1U);
  EXPECT_EQ(detached.compiler_ir().catalog_record_refs(),
            detached.deployment().catalog_records());
}

TEST(RetentionQualifiedDeployment,
     NonEmptyTrustBoundaryRejectsCatalogGraphAndApplicabilitySplices) {
  const auto fixture = test::make_runtime_only_retention_fixture();

  const auto catalog_splice = test::produce_runtime_only_deployment(
      fixture.runtime_profile, {},
      test::runtime_only_catalogs(fixture.runtime_profile,
                                  "sha256/runtime-only-profile-splice"));
  expect_mismatch(catalog_splice, fixture.compiler,
                  "catalog/source reference union");

  const auto graph_splice = test::produce_runtime_only_deployment(
      fixture.runtime_profile,
      test::runtime_only_graph(fixture.runtime_profile,
                               "runtime_only_splice"));
  expect_mismatch(graph_splice, fixture.compiler, "complete cell_graph");

  const auto applicability_splice = test::produce_runtime_only_deployment(
      fixture.runtime_profile, {}, {},
      std::string("[") + test::runtime_only_provider_binding() + "]");
  expect_mismatch(applicability_splice, fixture.compiler,
                  "applicability value tables");
}

}  // namespace
}  // namespace manyforge::deployment
