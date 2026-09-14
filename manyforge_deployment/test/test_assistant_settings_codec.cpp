#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
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
static_assert(
    !std::is_default_constructible_v<TypedAssistantSettingsCandidate>);
static_assert(!std::is_aggregate_v<QualifiedAssistantSettings>);
static_assert(!std::is_default_constructible_v<QualifiedAssistantSettings>);
static_assert(!std::is_aggregate_v<BuildQualifiedAssistantInstrumentRegistry>);

[[nodiscard]] std::string
mode(std::string_view mode_id = "query",
     std::string_view tools = "\"status_read\",\"scene_inspect\"",
     std::string_view skills = "", std::string_view nodes = "",
     std::string_view session_kind = "null",
     std::string_view approval_mode = "null",
     std::string_view review_policy = "not_required") {
  return "\"" + std::string(mode_id) + "\":{" + "\"enabled\":true," +
         "\"description\":\"Bounded mode\"," +
         "\"provider_alias\":\"direct_local\"," +
         "\"model_endpoint_alias\":\"primary_model\"," + "\"grant_policy\":{" +
         "\"session_kind\":" + std::string(session_kind) + "," +
         "\"approval_mode\":" + std::string(approval_mode) + "," +
         "\"review_policy\":\"" + std::string(review_policy) + "\"}," +
         "\"catalog\":{" + "\"skills\":[" + std::string(skills) + "]," +
         "\"tools\":[" + std::string(tools) + "]," + "\"nodes\":[" +
         std::string(nodes) + "]}}";
}

[[nodiscard]] std::string
model_request_contract(std::string_view supports_image_input = "true",
                       std::string_view supported_image_transports =
                           "\"http_url\",\"inline_data_url\"") {
  return "{\"default_timeout_s\":30.0,"
         "\"max_output_tokens\":512,"
         "\"supports_response_tools\":true,"
         "\"supports_json_object\":true,"
         "\"supports_image_input\":" +
         std::string(supports_image_input) +
         ",\"supported_image_transports\":[" +
         std::string(supported_image_transports) +
         "],\"tool_call_family\":\"openai_chat_tools\"}";
}

[[nodiscard]] std::string
model_alias(std::string request_contract = model_request_contract()) {
  return "\"primary_model\":{"
         "\"external_resource_alias\":\"local_model\","
         "\"model_id\":\"deployment-model\","
         "\"request_contract\":" +
         std::move(request_contract) + "}";
}

[[nodiscard]] std::string
assistant_document(std::string modes = mode(),
                   std::string_view provider_kind = "direct",
                   std::string_view assistant_extra = "",
                   std::string_view prompt_templates =
                       "\"choose_object_v1\":{"
                       "\"version\":\"1\","
                       "\"template_text\":\"Choose {{object_id}}\","
                       "\"variable_ids\":[\"object_id\"],"
                       "\"response_contract\":{"
                       "\"response_mode\":\"json_object\"}}") {
  return std::string(
             "{\"api_version\":\"manyforge.deployment.v1\",\"assistant\":{") +
         "\"audit_policy\":{"
         "\"store_full_prompts\":false,"
         "\"store_full_responses\":false,"
         "\"redact_argument_values\":true}," +
         "\"prompt_templates\":{" + std::string(prompt_templates) + "}," +
         "\"model_endpoint_aliases\":{" + model_alias() + "}," +
         "\"provider_aliases\":{" + "\"direct_local\":{" +
         "\"provider_kind\":\"" + std::string(provider_kind) + "\"," +
         "\"bridge_external_resource_alias\":\"assistant_bridge\"}}," +
         "\"modes\":{" + std::move(modes) + "}" + std::string(assistant_extra) +
         "}}";
}

[[nodiscard]] std::string
model_query_document(std::string_view prompt_templates =
                         "\"choose_object_v1\":{"
                         "\"version\":\"1\","
                         "\"template_text\":\"Choose {{object_id}}\","
                         "\"variable_ids\":[\"object_id\"],"
                         "\"response_contract\":{"
                         "\"response_mode\":\"json_object\"}}",
                     std::string model_endpoint_aliases = model_alias(),
                     std::string_view provider_aliases = "") {
  return std::string(
             "{\"api_version\":\"manyforge.deployment.v1\",\"assistant\":{") +
         "\"audit_policy\":{"
         "\"store_full_prompts\":false,"
         "\"store_full_responses\":false,"
         "\"redact_argument_values\":true}," +
         "\"prompt_templates\":{" + std::string(prompt_templates) + "}," +
         "\"model_endpoint_aliases\":{" + std::string(model_endpoint_aliases) +
         "}," + "\"provider_aliases\":{" + std::string(provider_aliases) +
         "}," + "\"modes\":{}}}";
}

[[nodiscard]] core::ExternalResourceDTO
external_resource(std::string alias, core::ExternalResourceKind kind) {
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

[[nodiscard]] TypedAssistantSettingsCandidate
decode(std::string_view document) {
  return decode_assistant_settings_candidate(
      parse_strict_json_model_yaml(document));
}

void expect_decode_error(std::string document,
                         std::string_view expected_fragment) {
  try {
    (void)decode(document);
    FAIL() << "expected AssistantSettingsCodecError";
  } catch (const AssistantSettingsCodecError &error) {
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
  } catch (const AssistantSettingsCodecError &error) {
    EXPECT_NE(std::string_view(error.what()).find(expected_fragment),
              std::string_view::npos)
        << error.what();
  }
}

[[nodiscard]] CanonicalValue replace_member(const CanonicalValue &object_value,
                                            std::string_view member_name,
                                            CanonicalValue replacement) {
  CanonicalValue::Object object = object_value.as_object();
  const auto iterator = std::find_if(object.begin(), object.end(),
                                     [member_name](const auto &member) {
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
  const auto empty =
      decode("{\"api_version\":\"manyforge.deployment.v1\",\"assistant\":{}}");
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
      decode(assistant_document(
          mode("query", "\"status_read\",\"scene_inspect\",\"alarms_read\""))),
      ext);
  const auto right = qualify_assistant_settings(
      decode(assistant_document(
          mode("query", "\"alarms_read\",\"scene_inspect\",\"status_read\""),
          "nemoclaw")),
      ext);

  ASSERT_TRUE(left.enabled());
  ASSERT_EQ(left.value().prompt_templates.size(), 1U);
  EXPECT_TRUE(is_sha256_digest(
      left.value().prompt_templates.front().content_digest.value()));
  EXPECT_TRUE(left.value().audit_policy.redact_argument_values);
  EXPECT_FALSE(left.value().audit_policy.store_full_prompts);
  ASSERT_EQ(left.value().modes.size(), 1U);
  EXPECT_EQ(left.value().modes.front().catalog.tool_ids,
            (std::vector<std::string>{"alarms_read", "scene_inspect",
                                      "status_read"}));
  EXPECT_TRUE(is_sha256_digest(left.projection_digest().value()));
  EXPECT_EQ(left.retained_projection().canonical_json,
            canonical_json(left.normalized_projection()));
  EXPECT_NE(left.projection_digest(), right.projection_digest())
      << "provider kind is retained semantic configuration";

  const auto verified =
      verify_retained_assistant_projection(left.normalized_projection(), ext);
  EXPECT_EQ(verified.value(), left.value());
  EXPECT_EQ(verified.projection_digest(), left.projection_digest());
}

TEST(AssistantSettingsCodec,
     ModelQueryOnlySettingsAreEnabledAndRetainedWithoutInteractiveModes) {
  const auto ext = resources();
  const auto qualified = qualify_assistant_settings(
      decode(model_query_document()),
      std::span<const core::ExternalResourceDTO>(ext).subspan(1U));

  ASSERT_TRUE(qualified.enabled());
  EXPECT_TRUE(qualified.value().modes.empty());
  ASSERT_EQ(qualified.value().prompt_templates.size(), 1U);
  EXPECT_EQ(qualified.value()
                .prompt_templates.front()
                .response_contract.response_mode,
            core::AssistantPromptResponseMode::kJsonObject);
  EXPECT_FALSE(qualified.value()
                   .prompt_templates.front()
                   .response_contract.tool_name.has_value());
  ASSERT_EQ(qualified.value().model_endpoint_aliases.size(), 1U);
  const auto &request_contract =
      qualified.value().model_endpoint_aliases.front().request_contract;
  EXPECT_DOUBLE_EQ(request_contract.default_timeout_s, 30.0);
  EXPECT_EQ(request_contract.max_output_tokens, 512U);
  EXPECT_TRUE(request_contract.supports_response_tools);
  EXPECT_TRUE(request_contract.supports_json_object);
  EXPECT_TRUE(request_contract.supports_image_input);
  EXPECT_EQ(request_contract.supported_image_transports.size(), 2U);
  EXPECT_EQ(request_contract.tool_call_family,
            core::AssistantToolCallFamily::kOpenAiChatTools);
  EXPECT_TRUE(qualified.value().provider_aliases.empty());
  EXPECT_TRUE(is_sha256_digest(qualified.projection_digest().value()));

  const auto verified = verify_retained_assistant_projection(
      qualified.normalized_projection(),
      std::span<const core::ExternalResourceDTO>(ext).subspan(1U));
  EXPECT_TRUE(verified.enabled());
  EXPECT_EQ(verified.value(), qualified.value());
  EXPECT_EQ(verified.projection_digest(), qualified.projection_digest());
}

TEST(AssistantSettingsCodec,
     ReorderedSourceMappingsAndListsHaveOneProjectionIdentity) {
  const auto ext = resources();
  const std::string left_modes =
      mode("query", "\"status_read\",\"scene_inspect\"") + "," +
      mode("composer-assistant", "\"program_read\",\"catalog_read\"", "", "",
           "null", "null", "required");
  const std::string right_modes =
      mode("composer-assistant", "\"catalog_read\",\"program_read\"", "", "",
           "null", "null", "required") +
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
     BuildRegistryRetainsReadsAndStrictV1AuthoringInstruments) {
  const auto &registry = build_qualified_assistant_instrument_registry();
  EXPECT_EQ(registry.value().api_version,
            kAssistantInstrumentRegistryApiVersion);
  EXPECT_TRUE(is_sha256_digest(registry.value().registry_digest.value()));
  std::vector<std::string> skill_ids;
  for (const auto &skill : registry.value().skills) {
    skill_ids.push_back(skill.skill_id);
  }
  EXPECT_EQ(skill_ids,
            (std::vector<std::string>{"move_to_named_config", "pick_object",
                                      "place_object"}));
  std::vector<std::string> ids;
  std::vector<std::string> read_only;
  std::vector<std::string> proposals;
  std::vector<std::string> draft_mutations;
  std::size_t total_canonical_schema_bytes = 0U;
  for (const auto &tool : registry.value().tools) {
    ids.push_back(tool.tool_id);
    total_canonical_schema_bytes += tool.canonical_input_schema.size();
    EXPECT_EQ(tool.input_schema_digest.value(),
              sha256_digest(tool.canonical_input_schema));
    EXPECT_EQ(
        canonical_json(
            parse_strict_json_model_yaml(tool.canonical_input_schema).root()),
        tool.canonical_input_schema);
    switch (tool.effect) {
    case core::AssistantToolEffect::kReadOnly:
      read_only.push_back(tool.tool_id);
      break;
    case core::AssistantToolEffect::kProposal:
      proposals.push_back(tool.tool_id);
      break;
    case core::AssistantToolEffect::kComposerDraftMutating:
      draft_mutations.push_back(tool.tool_id);
      break;
    case core::AssistantToolEffect::kRuntimeMutating:
      FAIL() << "N6-b2 must not restore runtime-mutating assistant tools";
    }
  }
  EXPECT_LT(total_canonical_schema_bytes, 512U * 1024U);
  EXPECT_EQ(ids, (std::vector<std::string>{"alarms_read",
                                           "blackboard_draft_remove_keys",
                                           "blackboard_draft_remove_namespaces",
                                           "blackboard_draft_upsert_keys",
                                           "blackboard_draft_upsert_namespaces",
                                           "catalog_read",
                                           "deployment_capabilities_read",
                                           "history_read",
                                           "inspect_isaac_scene",
                                           "program_draft_remove_parameters",
                                           "program_draft_upsert_parameters",
                                           "program_read",
                                           "program_validate",
                                           "propose_scene_object_nodes",
                                           "propose_scene_objects",
                                           "scene_draft_add_object",
                                           "scene_draft_remove_objects",
                                           "scene_draft_update_object",
                                           "scene_draft_upsert_objects",
                                           "scene_inspect",
                                           "skills_read",
                                           "status_read",
                                           "tree_draft_change_node_kind",
                                           "tree_draft_delete_node",
                                           "tree_draft_insert_node",
                                           "tree_draft_move_node",
                                           "tree_draft_rename_node",
                                           "tree_draft_replace_subtree",
                                           "tree_draft_update_node_params",
                                           "tree_draft_wrap_node"}));
  EXPECT_EQ(
      read_only,
      (std::vector<std::string>{
          "alarms_read", "catalog_read", "deployment_capabilities_read",
          "history_read", "inspect_isaac_scene", "program_read",
          "program_validate", "scene_inspect", "skills_read", "status_read"}));
  EXPECT_EQ(proposals, (std::vector<std::string>{"propose_scene_object_nodes",
                                                 "propose_scene_objects"}));
  EXPECT_EQ(draft_mutations.size(), 18U);

  std::vector<std::string> node_ids;
  for (const auto &node : registry.value().nodes) {
    node_ids.push_back(node.node_id);
  }
  EXPECT_EQ(
      node_ids,
      (std::vector<std::string>{
          "fallback", "gripper.command_position", "inverter", "logic.set_bool",
          "logic.wait_for_bool", "motion.follow_cartesian_path",
          "motion.move_to_joint_target", "motion.move_to_pose", "parallel",
          "repeat", "retry", "scene.add_object", "scene.attach_carried_object",
          "scene.detach_carried_object", "scene.remove_object",
          "scene.update_object_pose", "scene.upsert_object", "sequence",
          "timer"}));

  const auto catalog = std::find_if(
      registry.value().tools.begin(), registry.value().tools.end(),
      [](const auto &tool) { return tool.tool_id == "catalog_read"; });
  ASSERT_NE(catalog, registry.value().tools.end());
  EXPECT_NE(catalog->canonical_input_schema.find("\"entry_ids\""),
            std::string::npos);
  EXPECT_EQ(catalog->canonical_input_schema.find("entryIds"),
            std::string::npos);
}

TEST(AssistantSettingsCodec, RejectsOpenShapesAndNonCanonicalInstrumentIds) {
  expect_decode_error(
      assistant_document(mode(), "direct", ",\"unexpected\":true"),
      "unknown field 'unexpected'");
  expect_decode_error(
      assistant_document(mode("query", "\"status_read\",\"status_read\"")),
      "duplicate instrument ID");
  expect_decode_error(assistant_document(mode("query", "\"status.read\"")),
                      "canonical lowercase underscore tool ID");
  expect_decode_error(model_query_document("", ""),
                      "must configure an interactive mode or a bounded "
                      "model-query endpoint and prompt");
  expect_decode_error(
      model_query_document("", model_alias()),
      "require at least one prompt template and model endpoint");
  expect_decode_error(
      model_query_document("\"choose_object_v1\":{"
                           "\"version\":\"1\","
                           "\"template_text\":\"Choose\","
                           "\"variable_ids\":[],"
                           "\"response_contract\":{"
                           "\"response_mode\":\"json_object\"}}",
                           model_alias(),
                           "\"direct_local\":{"
                           "\"provider_kind\":\"direct\","
                           "\"bridge_external_resource_alias\":"
                           "\"assistant_bridge\"}"),
      "provider aliases require at least one interactive "
      "assistant mode");
  expect_decode_error(
      model_query_document(
          "\"choose_object_v1\":{"
          "\"version\":\"1\","
          "\"template_text\":\"Choose\","
          "\"variable_ids\":[],"
          "\"response_contract\":{"
          "\"response_mode\":\"json_object\"}}",
          model_alias(model_request_contract("false", "\"http_url\""))),
      "supports_image_input must match the presence of a supported image "
      "transport");
  expect_decode_error(
      model_query_document(
          "\"choose_object_v1\":{"
          "\"version\":\"1\","
          "\"template_text\":\"Choose\","
          "\"variable_ids\":[],"
          "\"response_contract\":{\"response_mode\":\"tool_call\"}}"),
      "missing required field 'tool_name'");
  expect_decode_error(
      assistant_document(mode(), "direct", "",
                         "\"choose_object_v1\":{"
                         "\"version\":\"1\","
                         "\"template_text\":\"Choose\","
                         "\"variable_ids\":[\"object_id\",\"object_id\"],"
                         "\"response_contract\":{"
                         "\"response_mode\":\"json_object\"}}"),
      "duplicate instrument ID");
}

TEST(AssistantSettingsCodec, RejectsUnresolvedOrWrongKindExternalAliases) {
  const auto ext = resources();
  expect_qualification_error(assistant_document(),
                             std::span<const core::ExternalResourceDTO>{},
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
      assistant_document(
          mode("query", "\"status_read\"", "", "", "null", "null", "required")),
      ext, "reserved mode grant/review policy is not exact");
  expect_qualification_error(
      assistant_document(mode("query", "\"unknown_read\"")), ext,
      "unresolved tool ID");

  const auto composer = qualify_assistant_settings(
      decode(assistant_document(
          mode("composer-assistant", "\"program_read\",\"catalog_read\"", "",
               "\"scene.add_object\"", "null", "null", "required"))),
      ext);
  EXPECT_TRUE(composer.enabled());
  expect_qualification_error(
      assistant_document(mode("composer-assistant", "\"program_read\"", "",
                              "\"add_collision_object\"", "null", "null",
                              "required")),
      ext, "unresolved node ID 'add_collision_object'");

  const auto recovery = qualify_assistant_settings(
      decode(assistant_document(mode(
          "error-recovery", "\"status_read\"", "", "", "\"delegated_recovery\"",
          "\"policy_granted\"", "bounded_by_recovery_profile"))),
      ext);
  EXPECT_TRUE(recovery.enabled());
}

TEST(AssistantSettingsCodec, RetainedProjectionRejectsAnyTampering) {
  const auto ext = resources();
  const auto qualified =
      qualify_assistant_settings(decode(assistant_document()), ext);

  const CanonicalValue bad_digest =
      replace_member(qualified.normalized_projection(), "projection_digest",
                     CanonicalValue(std::string(kZeroDigest)));
  EXPECT_THROW((void)verify_retained_assistant_projection(bad_digest, ext),
               AssistantSettingsCodecError);

  const CanonicalValue bad_registry =
      replace_member(qualified.normalized_projection(), "instruments",
                     CanonicalValue(CanonicalValue::Object{}));
  EXPECT_THROW((void)verify_retained_assistant_projection(bad_registry, ext),
               AssistantSettingsCodecError);
}

} // namespace
} // namespace manyforge::deployment
