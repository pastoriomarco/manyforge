#include <gtest/gtest.h>

#include <algorithm>
#include <manyforge_core/types/assistant.hpp>
#include <manyforge_core/types/deployment_applicability.hpp>
#include <manyforge_deployment/artifact_codec.hpp>
#include <manyforge_deployment/assistant_settings_codec.hpp>
#include <manyforge_deployment/canonical_value.hpp>
#include <manyforge_deployment/content_identity.hpp>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace manyforge::deployment {
namespace {

constexpr std::string_view kZeroDigest =
    "sha256:0000000000000000000000000000000000000000000000000000000000000000";

static_assert(!std::is_aggregate_v<TypedAssistantSettingsCandidate>);
static_assert(!std::is_default_constructible_v<TypedAssistantSettingsCandidate>);
static_assert(!std::is_aggregate_v<QualifiedAssistantSettings>);
static_assert(!std::is_default_constructible_v<QualifiedAssistantSettings>);
static_assert(
    !std::is_aggregate_v<BuildQualifiedAssistantInstrumentRegistry>);

[[nodiscard]] std::string mode(
    std::string_view mode_id = "query",
    std::string_view tools = "\"status_read\",\"scene_inspect\"",
    std::string_view skills = "", std::string_view nodes = "",
    std::string_view session_kind = "null",
    std::string_view approval_mode = "null",
    std::string_view review_policy = "not_required") {
  return "\"" + std::string(mode_id) + "\":{" +
         "\"enabled\":true," +
         "\"description\":\"Bounded mode\"," +
         "\"provider_alias\":\"direct_local\"," +
         "\"model_endpoint_alias\":\"primary_model\"," +
         "\"grant_policy\":{" +
         "\"session_kind\":" + std::string(session_kind) + "," +
         "\"approval_mode\":" + std::string(approval_mode) + "," +
         "\"review_policy\":\"" + std::string(review_policy) + "\"}," +
         "\"catalog\":{" +
         "\"skills\":[" + std::string(skills) + "]," +
         "\"tools\":[" + std::string(tools) + "]," +
         "\"nodes\":[" + std::string(nodes) + "]}}";
}

[[nodiscard]] std::string assistant_document(
    std::string modes = mode(), std::string_view provider_kind = "direct",
    std::string_view assistant_extra = "") {
  return std::string(
             "{\"api_version\":\"manyforge.deployment.v1\",\"assistant\":{") +
         "\"model_endpoint_aliases\":{" +
         "\"primary_model\":{" +
         "\"external_resource_alias\":\"local_model\"," +
         "\"model_id\":\"deployment-model\"}}," +
         "\"provider_aliases\":{" +
         "\"direct_local\":{" +
         "\"provider_kind\":\"" + std::string(provider_kind) + "\"," +
         "\"bridge_external_resource_alias\":\"assistant_bridge\"}}," +
         "\"modes\":{" + std::move(modes) + "}" +
         std::string(assistant_extra) + "}}";
}

[[nodiscard]] core::ExternalResourceDTO external_resource(
    std::string alias, core::ExternalResourceKind kind) {
  return {
      core::ExternalResourceAlias(std::move(alias)),
      kind,
      core::ProviderInstanceId("provider"),
      core::ProviderManifestId("provider_manifest"),
      core::ProviderManifestRevision(std::string(kZeroDigest)),
      core::EndpointRoleId("endpoint"),
      core::SiteOverlayValueRef("resources.endpoint"),
  };
}

[[nodiscard]] std::vector<core::ExternalResourceDTO> resources() {
  return {
      external_resource("assistant_bridge",
                        core::ExternalResourceKind::kServiceEndpoint),
      external_resource("local_model",
                        core::ExternalResourceKind::kModelEndpoint),
  };
}

[[nodiscard]] TypedAssistantSettingsCandidate decode(
    std::string_view document) {
  return decode_assistant_settings_candidate(
      parse_strict_json_model_yaml(document));
}

void expect_decode_error(std::string document,
                         std::string_view expected_fragment) {
  try {
    (void)decode(document);
    FAIL() << "expected AssistantSettingsCodecError";
  } catch (const AssistantSettingsCodecError& error) {
    EXPECT_NE(std::string_view(error.what()).find(expected_fragment),
              std::string_view::npos)
        << error.what();
  }
}

void expect_qualification_error(std::string document,
                                std::span<const core::ExternalResourceDTO> ext,
                                std::string_view expected_fragment) {
  try {
    (void)qualify_assistant_settings(decode(document), ext);
    FAIL() << "expected AssistantSettingsCodecError";
  } catch (const AssistantSettingsCodecError& error) {
    EXPECT_NE(std::string_view(error.what()).find(expected_fragment),
              std::string_view::npos)
        << error.what();
  }
}

[[nodiscard]] CanonicalValue replace_member(
    const CanonicalValue& object_value, std::string_view member_name,
    CanonicalValue replacement) {
  CanonicalValue::Object object = object_value.as_object();
  const auto iterator = std::find_if(
      object.begin(), object.end(), [member_name](const auto& member) {
        return member.first == member_name;
      });
  EXPECT_NE(iterator, object.end());
  if (iterator != object.end()) {
    iterator->second = std::move(replacement);
  }
  return CanonicalValue(std::move(object));
}

TEST(AssistantSettingsCodec,
     AbsentAndExactEmptyObjectsAreDisabledButPreserveSourcePresence) {
  const auto absent = decode("{\"api_version\":\"manyforge.deployment.v1\"}");
  const auto empty = decode(
      "{\"api_version\":\"manyforge.deployment.v1\",\"assistant\":{}}");
  EXPECT_FALSE(absent.source_was_present());
  EXPECT_TRUE(empty.source_was_present());
  EXPECT_FALSE(absent.enabled());
  EXPECT_FALSE(empty.enabled());
  EXPECT_EQ(absent.value(), empty.value());
  EXPECT_EQ(absent.normalized_source(), empty.normalized_source());

  const auto ext = resources();
  const auto qualified = qualify_assistant_settings(empty, ext);
  EXPECT_FALSE(qualified.enabled());
  EXPECT_THROW((void)qualified.retained_projection(),
               AssistantSettingsCodecError);
}

TEST(AssistantSettingsCodec,
     QueryModeResolvesAliasesAndProducesDeterministicRetainedProjection) {
  const auto ext = resources();
  const auto left = qualify_assistant_settings(
      decode(assistant_document(mode(
          "query", "\"status_read\",\"scene_inspect\",\"alarms_read\""))),
      ext);
  const auto right = qualify_assistant_settings(
      decode(assistant_document(mode(
          "query", "\"alarms_read\",\"scene_inspect\",\"status_read\""),
                                "nemoclaw")),
      ext);

  ASSERT_TRUE(left.enabled());
  ASSERT_EQ(left.value().modes.size(), 1U);
  EXPECT_EQ(left.value().modes.front().catalog.tool_ids,
            (std::vector<std::string>{"alarms_read", "scene_inspect",
                                      "status_read"}));
  EXPECT_TRUE(is_sha256_digest(left.projection_digest().value()));
  EXPECT_EQ(left.retained_projection().canonical_json,
            canonical_json(left.normalized_projection()));
  EXPECT_NE(left.projection_digest(), right.projection_digest())
      << "provider kind is retained semantic configuration";

  const auto verified = verify_retained_assistant_projection(
      left.normalized_projection(), ext);
  EXPECT_EQ(verified.value(), left.value());
  EXPECT_EQ(verified.projection_digest(), left.projection_digest());
}

TEST(AssistantSettingsCodec,
     ReorderedSourceMappingsAndListsHaveOneProjectionIdentity) {
  const auto ext = resources();
  const std::string left_modes =
      mode("query", "\"status_read\",\"scene_inspect\"") + "," +
      mode("composer-assistant", "\"program_read\",\"catalog_read\"",
           "", "", "null", "null", "required");
  const std::string right_modes =
      mode("composer-assistant", "\"catalog_read\",\"program_read\"",
           "", "", "null", "null", "required") +
      "," + mode("query", "\"scene_inspect\",\"status_read\"");
  const auto left =
      qualify_assistant_settings(decode(assistant_document(left_modes)), ext);
  const auto right =
      qualify_assistant_settings(decode(assistant_document(right_modes)), ext);
  EXPECT_EQ(left.value(), right.value());
  EXPECT_EQ(left.normalized_projection(), right.normalized_projection());
  EXPECT_EQ(left.projection_digest(), right.projection_digest());
}

TEST(AssistantSettingsCodec,
     BuildRegistryRetainsHistoricalReadsAndBindsEveryInputSchema) {
  const auto& registry = build_qualified_assistant_instrument_registry();
  EXPECT_EQ(registry.value().api_version,
            kAssistantInstrumentRegistryApiVersion);
  EXPECT_TRUE(is_sha256_digest(registry.value().registry_digest.value()));
  EXPECT_TRUE(registry.value().skills.empty());
  EXPECT_TRUE(registry.value().nodes.empty());

  std::vector<std::string> ids;
  for (const auto& tool : registry.value().tools) {
    ids.push_back(tool.tool_id);
    EXPECT_EQ(tool.effect, core::AssistantToolEffect::kReadOnly);
    EXPECT_EQ(tool.input_schema_digest.value(),
              sha256_digest(tool.canonical_input_schema));
    EXPECT_EQ(canonical_json(parse_strict_json_model_yaml(
                                 tool.canonical_input_schema)
                                 .root()),
              tool.canonical_input_schema);
  }
  EXPECT_EQ(ids,
            (std::vector<std::string>{
                "alarms_read", "catalog_read",
                "deployment_capabilities_read", "history_read",
                "program_read", "scene_inspect", "skills_read",
                "status_read"}));
}

TEST(AssistantSettingsCodec, RejectsOpenShapesAndNonCanonicalInstrumentIds) {
  expect_decode_error(
      assistant_document(mode(), "direct", ",\"unexpected\":true"),
      "unknown field 'unexpected'");
  expect_decode_error(
      assistant_document(mode("query", "\"status_read\",\"status_read\"")),
      "duplicate instrument ID");
  expect_decode_error(
      assistant_document(mode("query", "\"status.read\"")),
      "canonical lowercase underscore tool ID");
  expect_decode_error(
      assistant_document(""),
      "must declare at least one mode");
}

TEST(AssistantSettingsCodec, RejectsUnresolvedOrWrongKindExternalAliases) {
  const auto ext = resources();
  expect_qualification_error(
      assistant_document(), std::span<const core::ExternalResourceDTO>{},
      "external_resource_alias does not resolve");

  auto wrong_model = ext;
  wrong_model[1] = external_resource(
      "local_model", core::ExternalResourceKind::kServiceEndpoint);
  expect_qualification_error(assistant_document(), wrong_model,
                             "is not a model_endpoint");

  auto wrong_bridge = ext;
  wrong_bridge[0] = external_resource(
      "assistant_bridge", core::ExternalResourceKind::kModelEndpoint);
  expect_qualification_error(assistant_document(), wrong_bridge,
                             "is not a service_endpoint");
}

TEST(AssistantSettingsCodec, EnforcesExactReservedModePolicies) {
  const auto ext = resources();
  expect_qualification_error(
      assistant_document(mode("query", "\"status_read\"", "", "", "null",
                              "null", "required")),
      ext, "reserved mode grant/review policy is not exact");
  expect_qualification_error(
      assistant_document(mode("query", "\"unknown_read\"")), ext,
      "unresolved tool ID");

  const auto composer = qualify_assistant_settings(
      decode(assistant_document(mode("composer-assistant",
                                     "\"program_read\",\"catalog_read\"",
                                     "", "", "null", "null", "required"))),
      ext);
  EXPECT_TRUE(composer.enabled());

  const auto recovery = qualify_assistant_settings(
      decode(assistant_document(mode(
          "error-recovery", "\"status_read\"", "", "",
          "\"delegated_recovery\"", "\"policy_granted\"",
          "bounded_by_recovery_profile"))),
      ext);
  EXPECT_TRUE(recovery.enabled());
}

TEST(AssistantSettingsCodec, RetainedProjectionRejectsAnyTampering) {
  const auto ext = resources();
  const auto qualified =
      qualify_assistant_settings(decode(assistant_document()), ext);

  const CanonicalValue bad_digest = replace_member(
      qualified.normalized_projection(), "projection_digest",
      CanonicalValue(std::string(kZeroDigest)));
  EXPECT_THROW((void)verify_retained_assistant_projection(bad_digest, ext),
               AssistantSettingsCodecError);

  const CanonicalValue bad_registry = replace_member(
      qualified.normalized_projection(), "instruments",
      CanonicalValue(CanonicalValue::Object{}));
  EXPECT_THROW((void)verify_retained_assistant_projection(bad_registry, ext),
               AssistantSettingsCodecError);
}

}  // namespace
}  // namespace manyforge::deployment
