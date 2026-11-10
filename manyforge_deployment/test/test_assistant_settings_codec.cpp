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
mode(std::string_view mode_id = "inspect",
     std::string_view tools = "\"status_read\",\"scene_inspect\"",
     std::string_view skills = "", std::string_view stuck_loop_recovery = "",
     std::string_view session_kind = "null",
     std::string_view approval_mode = "null",
     std::string_view review_policy = "not_required") {
  return "\"" + std::string(mode_id) + "\":{" + "\"enabled\":true," +
         "\"description\":\"Bounded mode\"," +
         "\"assistant_component_id\":\"@assistant@\"," +
         "\"model_endpoint_alias\":\"primary_model\"," +
         "\"runs_in\":[\"no_program_active\"]," + "\"grant_policy\":{" +
         "\"session_kind\":" + std::string(session_kind) + "," +
         "\"approval_mode\":" + std::string(approval_mode) + "," +
         "\"review_policy\":\"" + std::string(review_policy) + "\"}," +
         "\"catalog\":{" + "\"skills\":[" + std::string(skills) + "]," +
         "\"tools\":[" + std::string(tools) + "]}" +
         (stuck_loop_recovery.empty()
              ? std::string()
              : ",\"stuck_loop_recovery\":" + std::string(stuck_loop_recovery)) +
         "}";
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
                   std::string_view assistant_component_id = "assistant",
                   std::string_view assistant_extra = "",
                   std::string_view prompt_templates =
                       "\"choose_object_v1\":{"
                       "\"version\":\"1\","
                       "\"template_text\":\"Choose {{object_id}}\","
                       "\"variables\":{\"object_id\":\"string\"},"
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
         "\"modes\":{" +
         [&] {
           // Each mode names the document's assistant component.
           for (auto at = modes.find("@assistant@"); at != std::string::npos;
                at = modes.find("@assistant@"))
             modes.replace(at, 11, assistant_component_id);
           return modes;
         }() +
         "}" + std::string(assistant_extra) + "}}";
}

[[nodiscard]] std::string
model_query_document(std::string_view prompt_templates =
                         "\"choose_object_v1\":{"
                         "\"version\":\"1\","
                         "\"template_text\":\"Choose {{object_id}}\","
                         "\"variables\":{\"object_id\":\"string\"},"
                         "\"response_contract\":{"
                         "\"response_mode\":\"json_object\"}}",
                     std::string model_endpoint_aliases = model_alias()) {
  return std::string(
             "{\"api_version\":\"manyforge.deployment.v1\",\"assistant\":{") +
         "\"audit_policy\":{"
         "\"store_full_prompts\":false,"
         "\"store_full_responses\":false,"
         "\"redact_argument_values\":true}," +
         "\"prompt_templates\":{" + std::string(prompt_templates) + "}," +
         "\"model_endpoint_aliases\":{" + std::string(model_endpoint_aliases) +
         "}," + "\"modes\":{}}}";
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
                                std::string_view expected_fragment) {
  try {
    (void)qualify_assistant_settings(decode(document));
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

  const auto qualified = qualify_assistant_settings(empty);
  EXPECT_FALSE(qualified.enabled());
  EXPECT_THROW((void)qualified.retained_projection(),
               AssistantSettingsCodecError);
}

TEST(AssistantSettingsCodec, ModeRunStatesAreAClosedSortedNonEmptySet) {
  const auto with_runs_in = [](std::string_view runs_in) {
    auto modes = mode();
    modes.replace(modes.find("[\"no_program_active\"]"), 21, runs_in);
    return assistant_document(modes);
  };
  const auto qualified =
      qualify_assistant_settings(decode(assistant_document()));
  ASSERT_EQ(qualified.value().modes.size(), 1U);
  EXPECT_EQ(qualified.value().modes.front().runs_in,
            (std::vector<core::AssistantModeRunState>{
                core::AssistantModeRunState::kNoProgramActive}));
  EXPECT_EQ(qualified.value().modes.front().assistant_component_id, "assistant");
  expect_decode_error(with_runs_in("[]"), "runs in at least one cell state");
  expect_decode_error(with_runs_in("[\"during_execution\"]"),
                      "outside the closed registry");
  expect_decode_error(
      with_runs_in("[\"no_program_active\",\"no_program_active\"]"),
      "sorted and unique");
}

TEST(AssistantSettingsCodec,
     ReadOnlyModeProducesDeterministicRetainedProjection) {
  const auto left = qualify_assistant_settings(
      decode(assistant_document(
          mode("inspect", "\"status_read\",\"scene_inspect\",\"alarms_read\""))));
  const auto right = qualify_assistant_settings(
      decode(assistant_document(
          mode("inspect", "\"alarms_read\",\"scene_inspect\",\"status_read\""),
          "other_assistant")));

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
      << "the serving assistant component is retained semantic configuration";

  const auto verified =
      verify_retained_assistant_projection(left.normalized_projection());
  EXPECT_EQ(verified.value(), left.value());
  EXPECT_EQ(verified.projection_digest(), left.projection_digest());
}

TEST(AssistantSettingsCodec,
     ModelQueryOnlySettingsAreEnabledAndRetainedWithoutInteractiveModes) {
  // Model aliases resolve to no external resource (the AI model component).
  const auto qualified =
      qualify_assistant_settings(decode(model_query_document()));

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
  EXPECT_TRUE(is_sha256_digest(qualified.projection_digest().value()));

  const auto verified =
      verify_retained_assistant_projection(qualified.normalized_projection());
  EXPECT_TRUE(verified.enabled());
  EXPECT_EQ(verified.value(), qualified.value());
  EXPECT_EQ(verified.projection_digest(), qualified.projection_digest());
}

TEST(AssistantSettingsCodec,
     ReorderedSourceMappingsAndListsHaveOneProjectionIdentity) {
  const std::string left_modes =
      mode("inspect", "\"status_read\",\"scene_inspect\"") + "," +
      mode("composer-assistant", "\"program_read\",\"catalog_read\"", "", "",
           "null", "null", "required");
  const std::string right_modes =
      mode("composer-assistant", "\"catalog_read\",\"program_read\"", "", "",
           "null", "null", "required") +
      "," + mode("inspect", "\"scene_inspect\",\"status_read\"");
  const auto left =
      qualify_assistant_settings(decode(assistant_document(left_modes)));
  const auto right =
      qualify_assistant_settings(decode(assistant_document(right_modes)));
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
  for (const auto &tool : registry.value().tools) {
    ids.push_back(tool.tool_id);
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
      FAIL() << "the settings codec must not restore runtime-mutating assistant tools";
    }
  }
  EXPECT_EQ(ids, (std::vector<std::string>{"alarms_read",
                                           "blackboard_draft_remove_keys",
                                           "blackboard_draft_remove_namespaces",
                                           "blackboard_draft_upsert_keys",
                                           "blackboard_draft_upsert_namespaces",
                                           "catalog_read",
                                           "deployment_capabilities_read",
                                           "history_read",
                                           "inspect_isaac_scene",
                                           "program_draft_remove_contact_policies",
                                           "program_draft_remove_parameters",
                                           "program_draft_upsert_contact_policies",
                                           "program_draft_upsert_parameters",
                                           "program_read",
                                           "program_validate",
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
  // Assistant edits land in the Draft through the tool callback only.
  EXPECT_TRUE(proposals.empty());
  EXPECT_EQ(draft_mutations.size(), 20U);

  // Descriptions and input schemas are built from the node catalog where
  // the mode projection is built; the registry is identity and effect only.
  EXPECT_EQ(registry.normalized_root().find_object_member("nodes"), nullptr);
}

TEST(AssistantSettingsCodec, RejectsOpenShapesAndNonCanonicalInstrumentIds) {
  expect_decode_error(
      assistant_document(mode(), "assistant", ",\"unexpected\":true"),
      "unknown field 'unexpected'");
  expect_decode_error(
      assistant_document(mode("inspect", "\"status_read\",\"status_read\"")),
      "duplicate instrument ID");
  expect_decode_error(assistant_document(mode("inspect", "\"status.read\"")),
                      "canonical lowercase underscore tool ID");
  expect_decode_error(model_query_document("", ""),
                      "must configure an interactive mode or a bounded "
                      "model-query endpoint and prompt");
  expect_decode_error(
      model_query_document("", model_alias()),
      "require at least one prompt template and model endpoint");
  expect_decode_error(
      model_query_document(
          "\"choose_object_v1\":{"
          "\"version\":\"1\","
          "\"template_text\":\"Choose\","
          "\"variables\":{},"
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
          "\"variables\":{},"
          "\"response_contract\":{\"response_mode\":\"tool_call\"}}"),
      "missing required field 'tool_name'");
  expect_decode_error(
      assistant_document(mode(), "assistant", "",
                         "\"choose_object_v1\":{"
                         "\"version\":\"1\","
                         "\"template_text\":\"Choose\","
                         "\"variables\":{\"object_id\":\"stored_pose\"},"
                         "\"response_contract\":{"
                         "\"response_mode\":\"json_object\"}}"),
      "is outside the closed template variable types");
}

TEST(AssistantSettingsCodec, QualifiesWithoutExternalResources) {
  // The AI model component serves every model alias; nothing resolves to an
  // external resource or site overlay.
  const auto qualified = qualify_assistant_settings(decode(assistant_document(
      mode("composer-assistant", "\"program_read\"", "", "", "null", "null",
           "required"))));
  ASSERT_TRUE(qualified.enabled());
  ASSERT_EQ(qualified.value().model_endpoint_aliases.size(), 1U);
  EXPECT_EQ(qualified.value()
                .model_endpoint_aliases.front()
                .external_resource_alias.value(),
            "local_model");
  EXPECT_EQ(
      verify_retained_assistant_projection(qualified.normalized_projection())
          .projection_digest(),
      qualified.projection_digest());
}

TEST(AssistantSettingsCodec, EnforcesExactReservedModePolicies) {
  expect_qualification_error(
      assistant_document(mode("composer-assistant", "\"status_read\"")),
      "reserved mode grant/review policy is not exact");
  expect_qualification_error(
      assistant_document(mode("inspect", "\"unknown_read\"")),
      "unresolved tool ID");

  const auto composer = qualify_assistant_settings(
      decode(assistant_document(
          mode("composer-assistant", "\"program_read\",\"catalog_read\"", "",
               "", "null", "null", "required"))));
  EXPECT_TRUE(composer.enabled());
  EXPECT_TRUE(composer.value().modes.front().stuck_loop_recovery);
  const auto without_recovery = qualify_assistant_settings(
      decode(assistant_document(
          mode("composer-assistant", "\"program_read\"", "", "false", "null",
               "null", "required"))));
  EXPECT_FALSE(without_recovery.value().modes.front().stuck_loop_recovery);
  EXPECT_FALSE(verify_retained_assistant_projection(
                   without_recovery.normalized_projection())
                   .value()
                   .modes.front()
                   .stuck_loop_recovery);
  expect_decode_error(
      assistant_document(mode("composer-assistant", "\"program_read\"", "",
                              "\"on\"", "null", "null", "required")),
      "stuck_loop_recovery");

  const auto recovery = qualify_assistant_settings(
      decode(assistant_document(mode(
          "error-recovery", "\"status_read\"", "", "", "\"delegated_recovery\"",
          "\"policy_granted\"", "bounded_by_recovery_profile"))));
  EXPECT_TRUE(recovery.enabled());
}

TEST(AssistantSettingsCodec, RetainedProjectionRejectsAnyTampering) {
  const auto qualified =
      qualify_assistant_settings(decode(assistant_document()));

  const CanonicalValue bad_digest =
      replace_member(qualified.normalized_projection(), "projection_digest",
                     CanonicalValue(std::string(kZeroDigest)));
  EXPECT_THROW((void)verify_retained_assistant_projection(bad_digest),
               AssistantSettingsCodecError);

  const CanonicalValue bad_registry =
      replace_member(qualified.normalized_projection(), "instruments",
                     CanonicalValue(CanonicalValue::Object{}));
  EXPECT_THROW((void)verify_retained_assistant_projection(bad_registry),
               AssistantSettingsCodecError);
}

TEST(AssistantSettingsCodec, ExperimentalReasoningSwitchIsOptionalAndRetained) {
  const auto with = [](std::string_view value) {
    std::string contract = model_request_contract();
    contract.insert(1, "\"reasoning\":" + std::string(value) + ",");
    return model_query_document(
        "\"choose_object_v1\":{\"version\":\"1\","
        "\"template_text\":\"Choose {{object_id}}\","
        "\"variables\":{\"object_id\":\"string\"},"
        "\"response_contract\":{\"response_mode\":\"json_object\"}}",
        model_alias(contract));
  };
  const auto absent = qualify_assistant_settings(decode(model_query_document()));
  EXPECT_FALSE(absent.value().model_endpoint_aliases.front().request_contract.reasoning);
  const auto off = qualify_assistant_settings(decode(with("\"off\"")));
  ASSERT_TRUE(off.value().model_endpoint_aliases.front().request_contract.reasoning);
  EXPECT_FALSE(*off.value().model_endpoint_aliases.front().request_contract.reasoning);
  EXPECT_EQ(verify_retained_assistant_projection(off.normalized_projection())
                .projection_digest(),
            off.projection_digest());
  expect_decode_error(with("\"maybe\""), "reasoning must be on or off");
}

TEST(AssistantSettingsCodec, SamplingIsOptionalBoundedAndRetained) {
  const auto with = [](std::string_view value) {
    std::string contract = model_request_contract();
    contract.insert(1, "\"sampling\":" + std::string(value) + ",");
    return model_query_document(
        "\"choose_object_v1\":{\"version\":\"1\","
        "\"template_text\":\"Choose {{object_id}}\","
        "\"variables\":{\"object_id\":\"string\"},"
        "\"response_contract\":{\"response_mode\":\"json_object\"}}",
        model_alias(contract));
  };
  const auto absent = qualify_assistant_settings(decode(model_query_document()));
  EXPECT_FALSE(absent.value().model_endpoint_aliases.front().request_contract.sampling);
  const auto set = qualify_assistant_settings(
      decode(with("{\"temperature\":0.7,\"top_k\":20}")));
  const auto& sampling =
      set.value().model_endpoint_aliases.front().request_contract.sampling;
  ASSERT_TRUE(sampling);
  EXPECT_EQ(sampling->temperature, std::optional<double>(0.7));
  EXPECT_FALSE(sampling->top_p);
  EXPECT_EQ(sampling->top_k, std::optional<std::uint32_t>(20U));
  EXPECT_EQ(verify_retained_assistant_projection(set.normalized_projection())
                .projection_digest(),
            set.projection_digest());
  EXPECT_NO_THROW((void)decode(with("{\"temperature\":0}")));
  expect_decode_error(with("{}"), "sampling must set at least one field");
  expect_decode_error(with("{\"min_p\":0.1}"), "unknown field 'min_p'");
  expect_decode_error(with("{\"temperature\":2.5}"), "from 0 to 2");
  expect_decode_error(with("{\"temperature\":-0.1}"), "from 0 to 2");
  expect_decode_error(with("{\"top_p\":0}"), "within its fixed bound");
  expect_decode_error(with("{\"top_p\":1.5}"), "within its fixed bound");
  expect_decode_error(with("{\"top_k\":0}"), "within its fixed bound");
  expect_decode_error(with("{\"top_k\":1.5}"), "expected an integer");
}

TEST(AssistantSettingsCodec, ExperimentalCompactHistoryIsOptionalAndRetained) {
  const auto with = [](std::string_view value) {
    std::string composer = mode("composer-assistant", "\"program_read\"", "",
                                "", "null", "null", "required");
    composer.insert(composer.size() - 1,
                    ",\"compact_history\":" + std::string(value));
    return assistant_document(composer);
  };
  const auto absent = qualify_assistant_settings(decode(assistant_document(
      mode("composer-assistant", "\"program_read\"", "", "", "null", "null",
           "required"))));
  EXPECT_FALSE(absent.value().modes.front().compact_history);
  // Off is not encoded, so settings without the switch keep their identity.
  EXPECT_EQ(canonical_json(absent.normalized_projection()).find("compact_history"),
            std::string::npos);
  const auto off = qualify_assistant_settings(decode(with("false")));
  EXPECT_EQ(off.projection_digest(), absent.projection_digest());
  const auto on = qualify_assistant_settings(decode(with("true")));
  EXPECT_TRUE(on.value().modes.front().compact_history);
  EXPECT_NE(on.projection_digest(), absent.projection_digest());
  const auto retained =
      verify_retained_assistant_projection(on.normalized_projection());
  EXPECT_TRUE(retained.value().modes.front().compact_history);
  EXPECT_EQ(retained.projection_digest(), on.projection_digest());
  expect_decode_error(with("\"on\""), "compact_history");
}

TEST(AssistantSettingsCodec, ExperimentalConstrainedRegenerationIsOptionalAndRetained) {
  const auto with = [](std::string_view fields) {
    std::string composer = mode("composer-assistant", "\"program_read\"", "",
                                "", "null", "null", "required");
    composer.insert(composer.size() - 1, std::string(fields));
    return assistant_document(composer);
  };
  const auto absent = qualify_assistant_settings(decode(with("")));
  EXPECT_FALSE(absent.value().modes.front().constrained_regeneration);
  // Off is not encoded, so settings without the switch keep their identity.
  EXPECT_EQ(canonical_json(absent.normalized_projection())
                .find("constrained_regeneration"),
            std::string::npos);
  const auto off = qualify_assistant_settings(
      decode(with(",\"constrained_regeneration\":false")));
  EXPECT_EQ(off.projection_digest(), absent.projection_digest());
  // Alone and next to compact_history (both encoded in sorted order).
  for (const std::string_view fields :
       {",\"constrained_regeneration\":true",
        ",\"compact_history\":true,\"constrained_regeneration\":true"}) {
    const auto on = qualify_assistant_settings(decode(with(fields)));
    EXPECT_TRUE(on.value().modes.front().constrained_regeneration);
    EXPECT_NE(on.projection_digest(), absent.projection_digest());
    const auto retained =
        verify_retained_assistant_projection(on.normalized_projection());
    EXPECT_TRUE(retained.value().modes.front().constrained_regeneration);
    EXPECT_EQ(retained.projection_digest(), on.projection_digest());
  }
  expect_decode_error(with(",\"constrained_regeneration\":\"on\""),
                      "constrained_regeneration");
}

} // namespace
} // namespace manyforge::deployment
