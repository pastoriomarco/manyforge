#include <gtest/gtest.h>

#include <manyforge_core/types/deployment_applicability.hpp>
#include <manyforge_deployment/artifact_codec.hpp>
#include <manyforge_deployment/canonical_value.hpp>
#include <manyforge_deployment/content_identity.hpp>
#include <manyforge_deployment/site_overlay_resolution.hpp>

#include <algorithm>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace manyforge::deployment {
namespace {

constexpr std::string_view kZeroDigest =
    "sha256:0000000000000000000000000000000000000000000000000000000000000000";
constexpr std::string_view kOneDigest =
    "sha256:0000000000000000000000000000000000000000000000000000000000000001";
constexpr std::string_view kTwoDigest =
    "sha256:0000000000000000000000000000000000000000000000000000000000000002";

static_assert(!std::is_aggregate_v<VerifiedSiteOverlayArtifactProjection>);
static_assert(
    !std::is_default_constructible_v<VerifiedSiteOverlayArtifactProjection>);
static_assert(!std::is_aggregate_v<QualifiedSelectedSiteOverlay>);
static_assert(!std::is_default_constructible_v<QualifiedSelectedSiteOverlay>);

struct AuthoredOverlay final {
  core::SiteOverlayRefDTO reference;
  std::string stored_bytes;
};

[[nodiscard]] std::string string_value(std::string_view value) {
  return "{\"value_kind\":\"string\",\"value\":\"" +
         std::string(value) + "\"}";
}

[[nodiscard]] std::string boolean_value(bool value) {
  return std::string("{\"value_kind\":\"boolean\",\"value\":") +
         (value ? "true}" : "false}");
}

[[nodiscard]] std::string integer_value(std::string_view value) {
  return "{\"value_kind\":\"integer\",\"value\":" +
         std::string(value) + "}";
}

[[nodiscard]] std::string number_value(std::string_view value) {
  return "{\"value_kind\":\"number\",\"value\":" +
         std::string(value) + "}";
}

[[nodiscard]] std::string string_list_value(
    std::string values = "\"a\",\"b\"") {
  return "{\"value_kind\":\"string_list\",\"value\":[" +
         std::move(values) + "]}";
}

[[nodiscard]] std::string secret_reference_value(
    std::string_view reference = "vault://robot/token#current") {
  return "{\"value_kind\":\"secret_reference\","
         "\"secret_reference_id\":\"" +
         std::string(reference) + "\"}";
}

[[nodiscard]] std::string external_row(
    std::string_view value_ref, std::string value = string_value("value"),
    std::string_view resource_kind = "service_endpoint") {
  return "{\"site_overlay_value_ref\":\"" + std::string(value_ref) +
         "\",\"semantic_kind\":\"external_resource\","
         "\"resource_kind\":\"" +
         std::string(resource_kind) + "\",\"value\":" + std::move(value) +
         "}";
}

[[nodiscard]] std::string configuration_row(
    std::string_view value_ref, std::string value = string_value("value")) {
  return "{\"site_overlay_value_ref\":\"" + std::string(value_ref) +
         "\",\"semantic_kind\":\"configuration_value\",\"value\":" +
         std::move(value) + "}";
}

[[nodiscard]] std::string artifact_document(
    std::string_view id, std::string_view revision,
    std::string values) {
  return "{\"api_version\":\"manyforge.site_overlay.v1\","
         "\"site_overlay_id\":\"" +
         std::string(id) + "\",\"site_overlay_revision\":\"" +
         std::string(revision) + "\",\"values\":[" + std::move(values) +
         "]}";
}

[[nodiscard]] const std::string& row_ref(const CanonicalValue& row) {
  const CanonicalValue* value =
      row.find_object_member("site_overlay_value_ref");
  EXPECT_NE(value, nullptr);
  const auto* text =
      value == nullptr ? nullptr : std::get_if<std::string>(&value->storage());
  EXPECT_NE(text, nullptr);
  static const std::string empty;
  return text == nullptr ? empty : *text;
}

[[nodiscard]] AuthoredOverlay author_overlay(
    std::string values, std::string_view id = "plant_a") {
  const std::string provisional =
      artifact_document(id, kZeroDigest, values);
  const ParsedArtifactCandidate parsed =
      parse_strict_json_model_yaml(provisional);
  const CanonicalValue* source_values =
      parsed.root().find_object_member("values");
  EXPECT_NE(source_values, nullptr);
  auto normalized_values =
      std::get<CanonicalValue::Array>(source_values->storage());
  std::sort(normalized_values.begin(), normalized_values.end(),
            [](const auto& left, const auto& right) {
              return row_ref(left) < row_ref(right);
            });

  CanonicalValue payload(CanonicalValue::Object{
      {"api_version", CanonicalValue("manyforge.site_overlay.v1")},
      {"site_overlay_id", CanonicalValue(std::string(id))},
      {"values", CanonicalValue(normalized_values)},
  });
  std::string preimage{"manyforge.content-identity.v1"};
  preimage.push_back('\0');
  preimage.append("site-overlay-revision");
  preimage.push_back('\0');
  preimage.append("manyforge.site_overlay.v1");
  preimage.push_back('\0');
  preimage.append(canonical_json(payload));
  const std::string revision = sha256_digest(preimage);

  CanonicalValue normalized_root(CanonicalValue::Object{
      {"api_version", CanonicalValue("manyforge.site_overlay.v1")},
      {"site_overlay_id", CanonicalValue(std::string(id))},
      {"site_overlay_revision", CanonicalValue(revision)},
      {"values", CanonicalValue(std::move(normalized_values))},
  });
  const std::string digest = sha256_digest(canonical_json(normalized_root));
  return {
      {
          core::SiteOverlayId(std::string(id)),
          core::SiteOverlayRevision(revision),
          core::ContentDigest(digest),
      },
      artifact_document(id, revision, std::move(values)),
  };
}

[[nodiscard]] core::SiteOverlayExternalResourceKeyDTO key(
    std::string_view value_ref,
    core::ExternalResourceKind kind =
        core::ExternalResourceKind::kServiceEndpoint) {
  return {
      core::SiteOverlayValueRef(std::string(value_ref)),
      kind,
  };
}

[[nodiscard]] core::ExternalResourceDTO external_resource(
    std::string_view alias, std::string_view value_ref,
    core::ExternalResourceKind kind =
        core::ExternalResourceKind::kServiceEndpoint) {
  return {
      core::ExternalResourceAlias(std::string(alias)),
      kind,
      core::ProviderInstanceId("provider"),
      core::ProviderManifestId("manifest"),
      core::ProviderManifestRevision(std::string(kZeroDigest)),
      core::EndpointRoleId("endpoint"),
      core::SiteOverlayValueRef(std::string(value_ref)),
  };
}

[[nodiscard]] core::SiteOverlayRefDTO copy_reference(
    const core::SiteOverlayRefDTO& source) {
  return {
      core::SiteOverlayId(source.site_overlay_id.value()),
      core::SiteOverlayRevision(source.site_overlay_revision.value()),
      core::ContentDigest(source.non_secret_digest.value()),
  };
}

void expect_resolution_error_containing(
    const core::SiteOverlayRefDTO& reference, std::string_view stored_bytes,
    std::string_view expected_fragment) {
  try {
    (void)resolve_and_verify_site_overlay_artifact_projection(reference,
                                                               stored_bytes);
    FAIL() << "expected SiteOverlayResolutionError";
  } catch (const SiteOverlayResolutionError& error) {
    EXPECT_NE(std::string_view(error.what()).find(expected_fragment),
              std::string_view::npos)
        << error.what();
  }
}

TEST(SiteOverlayResolution,
     VerifiesAllValueKindsAndProjectsOnlyExternalResources) {
  const AuthoredOverlay authored = author_overlay(
      external_row("a.boolean", boolean_value(true), "model_endpoint") + "," +
      external_row("b.integer", integer_value("7"), "content_store") + "," +
      configuration_row("c.number", number_value("1.25")) + "," +
      configuration_row("d.list", string_list_value()) + "," +
      configuration_row("e.secret", secret_reference_value()) + "," +
      external_row("f.string", string_value("http://robot:18333"),
                   "service_endpoint"));

  const auto verified = resolve_and_verify_site_overlay_artifact_projection(
      authored.reference, authored.stored_bytes);
  EXPECT_EQ(verified.reference(), authored.reference);
  const auto keys = verified.external_resource_keys();
  ASSERT_EQ(keys.size(), 3U);
  EXPECT_EQ(keys[0],
            key("a.boolean", core::ExternalResourceKind::kModelEndpoint));
  EXPECT_EQ(keys[1],
            key("b.integer", core::ExternalResourceKind::kContentStore));
  EXPECT_EQ(keys[2],
            key("f.string", core::ExternalResourceKind::kServiceEndpoint));
}

TEST(SiteOverlayResolution,
     EquivalentSourceOrderHasOneRevisionDigestAndProjection) {
  const std::string first =
      external_row("a.endpoint") + "," +
      configuration_row("z.path", string_value("/opt/models"));
  const std::string second =
      configuration_row("z.path", string_value("/opt/models")) + "," +
      external_row("a.endpoint");
  const AuthoredOverlay forward = author_overlay(first);
  const AuthoredOverlay reverse = author_overlay(second);
  EXPECT_EQ(forward.reference, reverse.reference);

  const auto forward_verified =
      resolve_and_verify_site_overlay_artifact_projection(
          forward.reference, forward.stored_bytes);
  const auto reverse_verified =
      resolve_and_verify_site_overlay_artifact_projection(
          reverse.reference, reverse.stored_bytes);
  EXPECT_EQ(
      std::vector(forward_verified.external_resource_keys().begin(),
                  forward_verified.external_resource_keys().end()),
      std::vector(reverse_verified.external_resource_keys().begin(),
                  reverse_verified.external_resource_keys().end()));
}

TEST(SiteOverlayResolution,
     RejectsOpenOrMalformedRootRowAndConditionalUnionShapes) {
  const core::SiteOverlayRefDTO untrusted{
      core::SiteOverlayId("plant_a"),
      core::SiteOverlayRevision(std::string(kZeroDigest)),
      core::ContentDigest(std::string(kZeroDigest)),
  };
  EXPECT_THROW(
      (void)resolve_and_verify_site_overlay_artifact_projection(
          untrusted,
          "{\"api_version\":\"manyforge.site_overlay.v0\","
          "\"site_overlay_id\":\"plant_a\","
          "\"site_overlay_revision\":\"" +
              std::string(kZeroDigest) + "\",\"values\":[]}"),
      SiteOverlayResolutionError);
  EXPECT_THROW(
      (void)resolve_and_verify_site_overlay_artifact_projection(
          untrusted,
          "{\"api_version\":\"manyforge.site_overlay.v1\","
          "\"site_overlay_id\":\"plant_a\","
          "\"site_overlay_revision\":\"" +
              std::string(kZeroDigest) +
              "\",\"values\":[],\"unknown\":true}"),
      SiteOverlayResolutionError);

  for (const std::string& malformed : {
           "{\"site_overlay_value_ref\":\"endpoint\","
           "\"semantic_kind\":\"external_resource\",\"value\":" +
               string_value("x") + "}",
           "{\"site_overlay_value_ref\":\"setting\","
           "\"semantic_kind\":\"configuration_value\","
           "\"resource_kind\":\"data_source\",\"value\":" +
               string_value("x") + "}",
           "{\"site_overlay_value_ref\":\"setting\","
           "\"semantic_kind\":\"unknown\",\"value\":" +
               string_value("x") + "}",
           std::string(
               "{\"site_overlay_value_ref\":\"setting\","
               "\"semantic_kind\":\"configuration_value\","
               "\"value\":{\"value_kind\":\"secret_reference\","
               "\"value\":\"secret\"}}"),
       }) {
    EXPECT_THROW(
        (void)resolve_and_verify_site_overlay_artifact_projection(
            untrusted,
            artifact_document("plant_a", kZeroDigest, malformed)),
        SiteOverlayResolutionError);
  }
}

TEST(SiteOverlayResolution,
     RejectsMalformedValueAlternativesAndPortableReferences) {
  const core::SiteOverlayRefDTO untrusted{
      core::SiteOverlayId("plant_a"),
      core::SiteOverlayRevision(std::string(kZeroDigest)),
      core::ContentDigest(std::string(kZeroDigest)),
  };
  for (const std::string& malformed : {
           configuration_row("bad/in-row"),
           configuration_row(
               "setting", "{\"value_kind\":\"boolean\",\"value\":\"true\"}"),
           configuration_row(
               "setting", "{\"value_kind\":\"integer\",\"value\":1.25}"),
           configuration_row(
               "setting",
               "{\"value_kind\":\"string_list\",\"value\":[\"a\",1]}"),
           configuration_row(
               "setting", "{\"value_kind\":\"unknown\",\"value\":\"x\"}"),
           external_row("setting", string_value("x"), "unknown_kind"),
           configuration_row(
               "setting",
               "{\"value_kind\":\"string\",\"value\":\"x\","
               "\"unknown\":true}"),
       }) {
    EXPECT_THROW(
        (void)resolve_and_verify_site_overlay_artifact_projection(
            untrusted,
            artifact_document("plant_a", kZeroDigest, malformed)),
        SiteOverlayResolutionError);
  }

  EXPECT_THROW(
      (void)resolve_and_verify_site_overlay_artifact_projection(
          untrusted,
          artifact_document(
              "plant_a", kZeroDigest,
              configuration_row("a" + std::string(128U, 'z')))),
      SiteOverlayResolutionError);
}

TEST(SiteOverlayResolution,
     StringListCeilingAccepts4096AndRejects4097) {
  std::string boundary_items;
  for (std::size_t index = 0; index < core::kMaxSiteOverlayValueRows;
       ++index) {
    if (!boundary_items.empty()) {
      boundary_items.push_back(',');
    }
    boundary_items += "\"item_" + std::to_string(index) + "\"";
  }

  const AuthoredOverlay boundary = author_overlay(configuration_row(
      "setting", string_list_value(boundary_items)));
  const auto accepted = resolve_and_verify_site_overlay_artifact_projection(
      boundary.reference, boundary.stored_bytes);
  EXPECT_TRUE(accepted.external_resource_keys().empty());

  const AuthoredOverlay over_limit = author_overlay(configuration_row(
      "setting",
      string_list_value(boundary_items + ",\"item_4096\"")));
  expect_resolution_error_containing(
      over_limit.reference, over_limit.stored_bytes,
      "$.values[0].value.value: string-list count exceeds");
}

TEST(SiteOverlayResolution,
     RejectsControlAndFormatCharactersButAcceptsOpaqueSecretReferences) {
  const AuthoredOverlay allowed =
      author_overlay(configuration_row(
          "secret", secret_reference_value("vault://cell/robot:key#v1")));
  EXPECT_NO_THROW(
      (void)resolve_and_verify_site_overlay_artifact_projection(
          allowed.reference, allowed.stored_bytes));

  for (const std::string& invalid_value : {
           string_value("line\\u000abreak"),
           string_list_value("\"plain\",\"zero\\u200bwidth\""),
           secret_reference_value("vault\\u0009secret"),
           secret_reference_value(""),
       }) {
    const AuthoredOverlay invalid =
        author_overlay(configuration_row("setting", invalid_value));
    EXPECT_THROW(
        (void)resolve_and_verify_site_overlay_artifact_projection(
            invalid.reference, invalid.stored_bytes),
        SiteOverlayResolutionError);
  }
}

TEST(SiteOverlayResolution,
     RejectsDuplicateReferencesAcrossSemanticKinds) {
  const core::SiteOverlayRefDTO untrusted{
      core::SiteOverlayId("plant_a"),
      core::SiteOverlayRevision(std::string(kZeroDigest)),
      core::ContentDigest(std::string(kZeroDigest)),
  };
  EXPECT_THROW(
      (void)resolve_and_verify_site_overlay_artifact_projection(
          untrusted,
          artifact_document(
              "plant_a", kZeroDigest,
              external_row("same.ref") + "," +
                  configuration_row("same.ref"))),
      SiteOverlayResolutionError);
}

TEST(SiteOverlayResolution,
     VerifiesSemanticRevisionExactReferenceAndNormalizedDigest) {
  const AuthoredOverlay authored =
      author_overlay(external_row("resources.endpoint"));
  EXPECT_NO_THROW(
      (void)resolve_and_verify_site_overlay_artifact_projection(
          authored.reference, "\n  " + authored.stored_bytes + "\n"));

  auto wrong_id = copy_reference(authored.reference);
  wrong_id.site_overlay_id = core::SiteOverlayId("plant_b");
  EXPECT_THROW(
      (void)resolve_and_verify_site_overlay_artifact_projection(
          wrong_id, authored.stored_bytes),
      SiteOverlayResolutionError);

  auto wrong_revision = copy_reference(authored.reference);
  wrong_revision.site_overlay_revision =
      core::SiteOverlayRevision(std::string(kTwoDigest));
  EXPECT_THROW(
      (void)resolve_and_verify_site_overlay_artifact_projection(
          wrong_revision, authored.stored_bytes),
      SiteOverlayResolutionError);

  auto wrong_digest = copy_reference(authored.reference);
  wrong_digest.non_secret_digest =
      core::ContentDigest(std::string(kTwoDigest));
  EXPECT_THROW(
      (void)resolve_and_verify_site_overlay_artifact_projection(
          wrong_digest, authored.stored_bytes),
      SiteOverlayResolutionError);

  std::string stale = authored.stored_bytes;
  const std::string old_payload = "\"value\":\"value\"";
  const std::size_t value_position = stale.find(old_payload);
  ASSERT_NE(value_position, std::string::npos);
  stale.replace(value_position, old_payload.size(),
                "\"value\":\"changed\"");
  EXPECT_THROW(
      (void)resolve_and_verify_site_overlay_artifact_projection(
          authored.reference, stale),
      SiteOverlayResolutionError);
}

TEST(SiteOverlayResolution,
     ValueCeilingAccepts4096AndRejects4097BeforeTypedAllocation) {
  std::string first_4096;
  for (std::size_t index = 0;
       index < core::kMaxSiteOverlayValueRows; ++index) {
    if (!first_4096.empty()) {
      first_4096.push_back(',');
    }
    first_4096 +=
        external_row("resources.endpoint_" + std::to_string(index));
  }
  const AuthoredOverlay boundary = author_overlay(first_4096);
  const auto accepted = resolve_and_verify_site_overlay_artifact_projection(
      boundary.reference, boundary.stored_bytes);
  EXPECT_EQ(accepted.external_resource_keys().size(),
            core::kMaxSiteOverlayValueRows);

  const std::string over_limit =
      first_4096 + "," + external_row("resources.endpoint_4096");
  EXPECT_THROW(
      (void)resolve_and_verify_site_overlay_artifact_projection(
          boundary.reference,
          artifact_document("plant_a", kZeroDigest, over_limit)),
      SiteOverlayResolutionError);
}

TEST(SiteOverlayResolution,
     SelectionImplementsTheCompletePresenceTruthTable) {
  const std::optional<core::SiteOverlayRefDTO> no_reference;
  const std::span<const core::ExternalResourceDTO> no_resources;
  const auto empty =
      select_site_overlay_for_deployment(no_reference, no_resources, nullptr);
  EXPECT_FALSE(empty.has_selection());
  EXPECT_EQ(empty.reference(), nullptr);
  EXPECT_TRUE(empty.external_resource_keys().empty());

  const AuthoredOverlay authored =
      author_overlay(external_row("resources.endpoint"));
  const auto selected = resolve_and_verify_site_overlay_artifact_projection(
      authored.reference, authored.stored_bytes);
  EXPECT_THROW(
      (void)select_site_overlay_for_deployment(no_reference, no_resources,
                                               &selected),
      SiteOverlayResolutionError);

  const std::vector<core::ExternalResourceDTO> one_resource{
      external_resource("endpoint", "resources.endpoint")};
  EXPECT_THROW(
      (void)select_site_overlay_for_deployment(no_reference, one_resource,
                                               nullptr),
      SiteOverlayResolutionError);

  const std::optional<core::SiteOverlayRefDTO> exact_reference{
      authored.reference};
  EXPECT_THROW(
      (void)select_site_overlay_for_deployment(exact_reference, no_resources,
                                               nullptr),
      SiteOverlayResolutionError);

  const auto exact_empty =
      select_site_overlay_for_deployment(exact_reference, no_resources,
                                         &selected);
  ASSERT_TRUE(exact_empty.has_selection());
  ASSERT_NE(exact_empty.reference(), nullptr);
  EXPECT_EQ(*exact_empty.reference(), authored.reference);

  const auto exact_rows =
      select_site_overlay_for_deployment(exact_reference, one_resource,
                                         &selected);
  ASSERT_TRUE(exact_rows.has_selection());
  EXPECT_EQ(exact_rows.external_resource_keys().size(), 1U);
}

TEST(SiteOverlayResolution,
     SelectionRejectsEachReferenceMismatchIndependently) {
  const AuthoredOverlay authored =
      author_overlay(external_row("resources.endpoint"));
  const auto selected = resolve_and_verify_site_overlay_artifact_projection(
      authored.reference, authored.stored_bytes);
  const std::span<const core::ExternalResourceDTO> no_resources;

  auto wrong_id = copy_reference(authored.reference);
  wrong_id.site_overlay_id = core::SiteOverlayId("plant_b");
  EXPECT_THROW(
      (void)select_site_overlay_for_deployment(wrong_id, no_resources,
                                               &selected),
      SiteOverlayResolutionError);

  auto wrong_revision = copy_reference(authored.reference);
  wrong_revision.site_overlay_revision =
      core::SiteOverlayRevision(std::string(kTwoDigest));
  EXPECT_THROW(
      (void)select_site_overlay_for_deployment(wrong_revision, no_resources,
                                               &selected),
      SiteOverlayResolutionError);

  auto wrong_digest = copy_reference(authored.reference);
  wrong_digest.non_secret_digest =
      core::ContentDigest(std::string(kTwoDigest));
  EXPECT_THROW(
      (void)select_site_overlay_for_deployment(wrong_digest, no_resources,
                                               &selected),
      SiteOverlayResolutionError);
}

TEST(SiteOverlayResolution,
     SelectionRequiresExactReferenceAndKindMembership) {
  const AuthoredOverlay authored = author_overlay(
      external_row("resources.model", string_value("model"),
                   "model_endpoint") +
      "," +
      external_row("resources.service", string_value("service"),
                   "service_endpoint") +
      "," + configuration_row("resources.not_projected"));
  const auto selected = resolve_and_verify_site_overlay_artifact_projection(
      authored.reference, authored.stored_bytes);
  const std::optional<core::SiteOverlayRefDTO> requested{
      authored.reference};

  const std::vector<core::ExternalResourceDTO> valid{
      external_resource("model", "resources.model",
                        core::ExternalResourceKind::kModelEndpoint),
      external_resource("service", "resources.service",
                        core::ExternalResourceKind::kServiceEndpoint),
  };
  EXPECT_NO_THROW(
      (void)select_site_overlay_for_deployment(requested, valid, &selected));

  const std::vector<core::ExternalResourceDTO> missing{
      external_resource("missing", "resources.missing")};
  EXPECT_THROW(
      (void)select_site_overlay_for_deployment(requested, missing, &selected),
      SiteOverlayResolutionError);

  const std::vector<core::ExternalResourceDTO> configuration_only{
      external_resource("setting", "resources.not_projected")};
  EXPECT_THROW(
      (void)select_site_overlay_for_deployment(
          requested, configuration_only, &selected),
      SiteOverlayResolutionError);

  const std::vector<core::ExternalResourceDTO> wrong_kind{
      external_resource("model", "resources.model",
                        core::ExternalResourceKind::kServiceEndpoint)};
  EXPECT_THROW(
      (void)select_site_overlay_for_deployment(requested, wrong_kind,
                                               &selected),
      SiteOverlayResolutionError);
}

TEST(SiteOverlayResolution,
     SelectionRejectsExternalResourceInputAboveTheBound) {
  const AuthoredOverlay authored =
      author_overlay(external_row("resources.endpoint"));
  const auto selected = resolve_and_verify_site_overlay_artifact_projection(
      authored.reference, authored.stored_bytes);
  std::vector<core::ExternalResourceDTO> resources;
  resources.reserve(core::kMaxDeploymentApplicabilityRows + 1U);
  for (std::size_t index = 0;
       index < core::kMaxDeploymentApplicabilityRows + 1U; ++index) {
    resources.push_back(external_resource(
        "resource_" + std::to_string(index), "resources.endpoint"));
  }
  EXPECT_THROW(
      (void)select_site_overlay_for_deployment(authored.reference, resources,
                                               &selected),
      SiteOverlayResolutionError);
}

}  // namespace
}  // namespace manyforge::deployment
