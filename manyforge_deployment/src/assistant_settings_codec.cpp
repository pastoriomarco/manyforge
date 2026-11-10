#include "manyforge_deployment/assistant_settings_codec.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "compiler_semantic_limits_internal.hpp"
#include "content_identity_internal.hpp"
#include "manyforge_deployment/content_identity.hpp"

namespace manyforge::deployment {

struct TypedAssistantSettingsCandidate::Data final {
  Data(bool source_was_present_in, core::AssistantSettingsDTO value_in,
       CanonicalValue normalized_source_in)
      : source_was_present(source_was_present_in),
        value(std::move(value_in)),
        normalized_source(std::move(normalized_source_in)) {}

  bool source_was_present{false};
  core::AssistantSettingsDTO value;
  CanonicalValue normalized_source;
};

struct BuildQualifiedAssistantInstrumentRegistry::Data final {
  Data(core::AssistantInstrumentRegistryDTO value_in,
       CanonicalValue normalized_root_in)
      : value(std::move(value_in)),
        normalized_root(std::move(normalized_root_in)) {}

  core::AssistantInstrumentRegistryDTO value;
  CanonicalValue normalized_root;
};

struct QualifiedAssistantSettings::Data final {
  Data(bool enabled_in, core::AssistantSettingsDTO value_in,
       core::AssistantInstrumentRegistryDTO instruments_in,
       CanonicalValue normalized_projection_in,
       core::ContentDigest projection_digest_in)
      : enabled(enabled_in),
        value(std::move(value_in)),
        instruments(std::move(instruments_in)),
        normalized_projection(std::move(normalized_projection_in)),
        projection_digest(std::move(projection_digest_in)) {}

  bool enabled{false};
  core::AssistantSettingsDTO value;
  core::AssistantInstrumentRegistryDTO instruments;
  CanonicalValue normalized_projection;
  core::ContentDigest projection_digest;
};

namespace {

constexpr std::string_view kZeroDigest =
    "sha256:0000000000000000000000000000000000000000000000000000000000000000";
constexpr std::size_t kMaximumAssistantRows = 4096U;
constexpr std::size_t kMaximumAssistantTextBytes = 1024U;
constexpr std::size_t kMaximumPromptTemplateBytes = 16384U;
constexpr double kMaximumModelRequestTimeoutSeconds = 3600.0;
constexpr std::uint32_t kMaximumModelOutputTokens = 1U << 20U;
constexpr double kMaximumSamplingTemperature = 2.0;
constexpr std::uint32_t kMaximumSamplingTopK = 1U << 20U;

const detail::ContentIdentitySpec kInstrumentRegistryIdentitySpec{
    "assistant-instrument-registry-digest",
    std::string(kAssistantInstrumentRegistryApiVersion),
    "registry_digest",
    detail::ContentIdentityRootShape::kDocument,
};
const detail::ContentIdentitySpec kAssistantProjectionIdentitySpec{
    "retained-assistant-projection-digest",
    std::string(kRetainedAssistantProjectionApiVersion),
    "projection_digest",
    detail::ContentIdentityRootShape::kDocument,
};

[[noreturn]] void fail(std::string_view path, std::string_view reason) {
  throw AssistantSettingsCodecError(std::string(path) + ": " +
                                    std::string(reason));
}

[[nodiscard]] std::string child_path(std::string_view parent,
                                     std::string_view child) {
  return std::string(parent) + "." + std::string(child);
}

[[nodiscard]] std::string index_path(std::string_view parent,
                                     std::size_t index) {
  return std::string(parent) + "[" + std::to_string(index) + "]";
}

[[nodiscard]] const CanonicalValue::Object& require_object(
    const CanonicalValue& value, std::string_view path) {
  const auto* object = std::get_if<CanonicalValue::Object>(&value.storage());
  if (object == nullptr) {
    fail(path, "expected object");
  }
  return *object;
}

[[nodiscard]] const CanonicalValue::Array& require_array(
    const CanonicalValue& value, std::string_view path) {
  const auto* array = std::get_if<CanonicalValue::Array>(&value.storage());
  if (array == nullptr) {
    fail(path, "expected array");
  }
  return *array;
}

[[nodiscard]] const std::string& require_string(const CanonicalValue& value,
                                                std::string_view path) {
  const auto* text = std::get_if<std::string>(&value.storage());
  if (text == nullptr) {
    fail(path, "expected string");
  }
  return *text;
}

[[nodiscard]] bool require_bool(const CanonicalValue& value,
                                std::string_view path) {
  const auto* boolean = std::get_if<bool>(&value.storage());
  if (boolean == nullptr) {
    fail(path, "expected boolean");
  }
  return *boolean;
}

[[nodiscard]] double require_bounded_positive_number(
    const CanonicalValue& value, std::string_view path, double maximum) {
  const auto* number = std::get_if<double>(&value.storage());
  if (number == nullptr || !std::isfinite(*number) || *number <= 0.0 ||
      *number > maximum) {
    fail(path, "expected a positive finite number within its fixed bound");
  }
  return *number;
}

[[nodiscard]] std::uint32_t require_bounded_positive_integer(
    const CanonicalValue& value, std::string_view path,
    std::uint32_t maximum) {
  const double number =
      require_bounded_positive_number(value, path, maximum);
  if (std::floor(number) != number) {
    fail(path, "expected an integer");
  }
  return static_cast<std::uint32_t>(number);
}

[[nodiscard]] const CanonicalValue* find_member(
    const CanonicalValue::Object& object, std::string_view name) noexcept {
  const auto iterator = std::find_if(
      object.begin(), object.end(),
      [name](const auto& member) { return member.first == name; });
  return iterator == object.end() ? nullptr : &iterator->second;
}

[[nodiscard]] const CanonicalValue& require_member(
    const CanonicalValue::Object& object, std::string_view name,
    std::string_view path) {
  const CanonicalValue* value = find_member(object, name);
  if (value == nullptr) {
    fail(path, "missing required field '" + std::string(name) + "'");
  }
  return *value;
}

void require_exact_fields(
    const CanonicalValue::Object& object,
    std::initializer_list<std::string_view> expected, std::string_view path) {
  for (const auto& [name, unused] : object) {
    (void)unused;
    if (std::find(expected.begin(), expected.end(), name) == expected.end()) {
      fail(path, "unknown field '" + name + "'");
    }
  }
  for (const std::string_view name : expected) {
    (void)require_member(object, name, path);
  }
}

// Like require_exact_fields, with fields that may be left out.
void require_fields(const CanonicalValue::Object& object,
                    std::initializer_list<std::string_view> required,
                    std::initializer_list<std::string_view> optional,
                    std::string_view path) {
  for (const auto& [name, unused] : object) {
    (void)unused;
    if (std::find(required.begin(), required.end(), name) == required.end() &&
        std::find(optional.begin(), optional.end(), name) == optional.end()) {
      fail(path, "unknown field '" + name + "'");
    }
  }
  for (const std::string_view name : required) {
    (void)require_member(object, name, path);
  }
}

[[nodiscard]] constexpr bool is_ascii_alphanumeric(
    unsigned char byte) noexcept {
  return (byte >= 'a' && byte <= 'z') ||
         (byte >= 'A' && byte <= 'Z') ||
         (byte >= '0' && byte <= '9');
}

[[nodiscard]] bool is_portable_token(std::string_view value) noexcept {
  if (value.empty() ||
      value.size() > detail::kMaximumPortableIdentityBytes ||
      !is_ascii_alphanumeric(static_cast<unsigned char>(value.front()))) {
    return false;
  }
  return std::all_of(value.begin() + 1, value.end(), [](unsigned char byte) {
    return is_ascii_alphanumeric(byte) || byte == '.' || byte == '_' ||
           byte == '-';
  });
}

[[nodiscard]] bool is_canonical_tool_id(std::string_view value) noexcept {
  if (value.empty() ||
      value.size() > detail::kMaximumPortableIdentityBytes ||
      value.front() < 'a' || value.front() > 'z') {
    return false;
  }
  return std::all_of(value.begin() + 1, value.end(), [](unsigned char byte) {
    return (byte >= 'a' && byte <= 'z') ||
           (byte >= '0' && byte <= '9') || byte == '_';
  });
}

[[nodiscard]] std::string parse_portable_token(const CanonicalValue& value,
                                               std::string_view path) {
  const std::string& text = require_string(value, path);
  if (!is_portable_token(text)) {
    fail(path,
         "expected a 1..128 byte portable token beginning with an ASCII "
         "alphanumeric");
  }
  return text;
}

[[nodiscard]] std::string parse_tool_id(const CanonicalValue& value,
                                        std::string_view path) {
  const std::string& text = require_string(value, path);
  if (!is_canonical_tool_id(text)) {
    fail(path,
         "expected a canonical lowercase underscore tool ID matching "
         "[a-z][a-z0-9_]{0,127}");
  }
  return text;
}

[[nodiscard]] std::string parse_bounded_text(const CanonicalValue& value,
                                             std::string_view path,
                                             bool allow_empty) {
  const std::string& text = require_string(value, path);
  if ((!allow_empty && text.empty()) ||
      text.size() > kMaximumAssistantTextBytes ||
      std::any_of(text.begin(), text.end(), [](unsigned char byte) {
        return byte == 0U || byte == 0x7fU || byte < 0x20U;
      })) {
    fail(path,
         "expected bounded NFC text without ASCII control characters");
  }
  return text;
}

[[nodiscard]] core::ContentDigest parse_digest(const CanonicalValue& value,
                                               std::string_view path) {
  const std::string& text = require_string(value, path);
  if (!is_sha256_digest(text)) {
    fail(path, "expected lowercase sha256:<64 hex> digest");
  }
  return core::ContentDigest(text);
}

template <typename Enum, typename Parser>
[[nodiscard]] std::optional<Enum> parse_nullable_enum(
    const CanonicalValue& value, std::string_view path, Parser parser) {
  if (std::holds_alternative<std::nullptr_t>(value.storage())) {
    return std::nullopt;
  }
  return parser(value, path);
}

[[nodiscard]] core::AssistantModeRunState parse_run_state(
    const CanonicalValue& value, std::string_view path) {
  const std::string& text = require_string(value, path);
  if (text == "no_program_active") {
    return core::AssistantModeRunState::kNoProgramActive;
  }
  fail(path, "run state is outside the closed registry");
}

[[nodiscard]] core::AssistantSessionKind parse_session_kind(
    const CanonicalValue& value, std::string_view path) {
  const std::string& text = require_string(value, path);
  if (text == "manual_intervention") {
    return core::AssistantSessionKind::kManualIntervention;
  }
  if (text == "delegated_recovery") {
    return core::AssistantSessionKind::kDelegatedRecovery;
  }
  fail(path, "session_kind is outside the closed v1 enum");
}

[[nodiscard]] core::AssistantApprovalMode parse_approval_mode(
    const CanonicalValue& value, std::string_view path) {
  const std::string& text = require_string(value, path);
  if (text == "human_approved") {
    return core::AssistantApprovalMode::kHumanApproved;
  }
  if (text == "policy_granted") {
    return core::AssistantApprovalMode::kPolicyGranted;
  }
  fail(path, "approval_mode is outside the closed v1 enum");
}

[[nodiscard]] core::AssistantReviewPolicy parse_review_policy(
    const CanonicalValue& value, std::string_view path) {
  const std::string& text = require_string(value, path);
  if (text == "required") {
    return core::AssistantReviewPolicy::kRequired;
  }
  if (text == "bounded_by_recovery_profile") {
    return core::AssistantReviewPolicy::kBoundedByRecoveryProfile;
  }
  if (text == "not_required") {
    return core::AssistantReviewPolicy::kNotRequired;
  }
  fail(path, "review_policy is outside the closed v1 enum");
}

[[nodiscard]] core::AssistantImageTransport parse_image_transport(
    const CanonicalValue& value, std::string_view path) {
  const std::string& text = require_string(value, path);
  if (text == "http_url") {
    return core::AssistantImageTransport::kHttpUrl;
  }
  if (text == "inline_data_url") {
    return core::AssistantImageTransport::kInlineDataUrl;
  }
  fail(path, "image transport is outside the closed v1 enum");
}

[[nodiscard]] core::AssistantToolCallFamily parse_tool_call_family(
    const CanonicalValue& value, std::string_view path) {
  const std::string& text = require_string(value, path);
  if (text == "openai_chat_tools") {
    return core::AssistantToolCallFamily::kOpenAiChatTools;
  }
  if (text == "qwen3_coder") {
    return core::AssistantToolCallFamily::kQwen3Coder;
  }
  if (text == "qwen3_xml") {
    return core::AssistantToolCallFamily::kQwen3Xml;
  }
  fail(path, "tool_call_family is outside the closed v1 enum");
}

[[nodiscard]] core::AssistantPromptResponseMode parse_prompt_response_mode(
    const CanonicalValue& value, std::string_view path) {
  const std::string& text = require_string(value, path);
  if (text == "json_object") {
    return core::AssistantPromptResponseMode::kJsonObject;
  }
  if (text == "tool_call") {
    return core::AssistantPromptResponseMode::kToolCall;
  }
  fail(path, "response_mode is outside the closed v1 enum");
}

enum class InstrumentIdKind { kPortable, kTool };

[[nodiscard]] std::vector<std::string> parse_id_list(
    const CanonicalValue& value, std::string_view path,
    InstrumentIdKind kind) {
  const auto& array = require_array(value, path);
  if (array.size() > kMaximumAssistantRows) {
    fail(path, "instrument list exceeds the 4096-row ceiling");
  }
  std::vector<std::string> result;
  result.reserve(array.size());
  for (std::size_t index = 0; index < array.size(); ++index) {
    result.push_back(kind == InstrumentIdKind::kTool
                         ? parse_tool_id(array[index], index_path(path, index))
                         : parse_portable_token(array[index],
                                                index_path(path, index)));
  }
  std::sort(result.begin(), result.end());
  if (std::adjacent_find(result.begin(), result.end()) != result.end()) {
    fail(path, "duplicate instrument ID");
  }
  return result;
}

[[nodiscard]] std::vector<core::AssistantPromptTemplateVariableDTO>
parse_template_variables(const CanonicalValue& value, std::string_view path) {
  const auto& object = require_object(value, path);
  if (object.size() > kMaximumAssistantRows) {
    fail(path, "template variables exceed the 4096-row ceiling");
  }
  std::vector<core::AssistantPromptTemplateVariableDTO> result;
  result.reserve(object.size());
  for (const auto& [variable_id, type] : object) {
    const std::string entry_path = child_path(path, variable_id);
    const std::string& value_type = require_string(type, entry_path);
    if (value_type != "bool" && value_type != "float64" &&
        value_type != "int64" && value_type != "string" &&
        value_type != "string_list") {
      fail(entry_path, "is outside the closed template variable types");
    }
    result.push_back({parse_portable_token(CanonicalValue(variable_id), entry_path),
                      value_type});
  }
  std::sort(result.begin(), result.end(), [](const auto& left, const auto& right) {
    return left.variable_id < right.variable_id;
  });
  if (std::adjacent_find(result.begin(), result.end(),
                         [](const auto& left, const auto& right) {
                           return left.variable_id == right.variable_id;
                         }) != result.end()) {
    fail(path, "duplicate template variable ID");
  }
  return result;
}

[[nodiscard]] CanonicalValue encode_template_variables(
    const std::vector<core::AssistantPromptTemplateVariableDTO>& variables) {
  CanonicalValue::Object result;
  result.reserve(variables.size());
  for (const auto& variable : variables) {
    result.emplace_back(variable.variable_id, CanonicalValue(variable.value_type));
  }
  return CanonicalValue(std::move(result));
}

[[nodiscard]] CanonicalValue encode_string_array(
    const std::vector<std::string>& values) {
  CanonicalValue::Array result;
  result.reserve(values.size());
  for (const auto& value : values) {
    result.emplace_back(value);
  }
  return CanonicalValue(std::move(result));
}

[[nodiscard]] CanonicalValue encode_image_transports(
    const std::vector<core::AssistantImageTransport>& transports) {
  CanonicalValue::Array result;
  result.reserve(transports.size());
  for (const auto transport : transports) {
    result.emplace_back(std::string(core::to_string(transport)));
  }
  return CanonicalValue(std::move(result));
}

[[nodiscard]] CanonicalValue encode_model_sampling(
    const core::AssistantModelSamplingDTO& sampling) {
  CanonicalValue::Object result;
  if (sampling.temperature.has_value()) {
    result.emplace_back("temperature", CanonicalValue(*sampling.temperature));
  }
  if (sampling.top_k.has_value()) {
    result.emplace_back("top_k",
                        CanonicalValue(static_cast<double>(*sampling.top_k)));
  }
  if (sampling.top_p.has_value()) {
    result.emplace_back("top_p", CanonicalValue(*sampling.top_p));
  }
  return CanonicalValue(std::move(result));
}

[[nodiscard]] CanonicalValue encode_model_request_contract(
    const core::AssistantModelEndpointRequestContractDTO& contract) {
  CanonicalValue::Object result{
      {"default_timeout_s", CanonicalValue(contract.default_timeout_s)},
      {"max_output_tokens",
       CanonicalValue(static_cast<double>(contract.max_output_tokens))},
      {"supported_image_transports",
       encode_image_transports(contract.supported_image_transports)},
      {"supports_image_input",
       CanonicalValue(contract.supports_image_input)},
      {"supports_json_object",
       CanonicalValue(contract.supports_json_object)},
      {"supports_response_tools",
       CanonicalValue(contract.supports_response_tools)},
      {"tool_call_family",
       CanonicalValue(std::string(core::to_string(contract.tool_call_family)))},
  };
  if (contract.reasoning.has_value()) {
    // EXPERIMENTAL; members stay in canonical (sorted) order.
    result.emplace(result.begin() + 2, "reasoning",
                   CanonicalValue(std::string(*contract.reasoning ? "on" : "off")));
  }
  if (contract.sampling.has_value()) {
    // The object constructor puts members in canonical order.
    result.emplace_back("sampling", encode_model_sampling(*contract.sampling));
  }
  return CanonicalValue(std::move(result));
}

[[nodiscard]] std::string parse_prompt_template_text(const CanonicalValue& value,
                                                     std::string_view path) {
  const std::string& text = require_string(value, path);
  if (text.empty() || text.size() > kMaximumPromptTemplateBytes ||
      std::any_of(text.begin(), text.end(), [](unsigned char byte) {
        return byte == 0U || byte == 0x7fU || (byte < 0x20U && byte != '\n' && byte != '\t');
      })) {
    fail(path,
         "expected a non-empty prompt template within the 16 KiB bound "
         "without unsupported control characters");
  }
  return text;
}

[[nodiscard]] CanonicalValue encode_prompt_response_contract(
    const core::AssistantPromptResponseContractDTO& contract) {
  CanonicalValue::Object result{
      {"response_mode",
       CanonicalValue(std::string(core::to_string(contract.response_mode)))},
  };
  if (contract.response_mode == core::AssistantPromptResponseMode::kToolCall) {
    if (!contract.tool_name.has_value() ||
        !contract.tool_description.has_value()) {
      fail("assistant.prompt_templates.response_contract",
           "tool_call response contract is incomplete");
    }
    result.emplace_back("tool_description",
                        CanonicalValue(*contract.tool_description));
    result.emplace_back("tool_name", CanonicalValue(*contract.tool_name));
  } else if (contract.tool_name.has_value() ||
             contract.tool_description.has_value()) {
    fail("assistant.prompt_templates.response_contract",
         "json_object response contract carries tool metadata");
  }
  return CanonicalValue(std::move(result));
}

[[nodiscard]] core::ContentDigest prompt_template_content_digest(
    std::string_view prompt_template_id, std::string_view version, std::string_view template_text,
    const std::vector<core::AssistantPromptTemplateVariableDTO>& variables,
    const core::AssistantPromptResponseContractDTO& response_contract) {
  const CanonicalValue content(CanonicalValue::Object{
      {"prompt_template_id", CanonicalValue(std::string(prompt_template_id))},
      {"response_contract", encode_prompt_response_contract(response_contract)},
      {"template_text", CanonicalValue(std::string(template_text))},
      {"variables", encode_template_variables(variables)},
      {"version", CanonicalValue(std::string(version))},
  });
  return core::ContentDigest(sha256_digest(canonical_json(content)));
}

[[nodiscard]] CanonicalValue replace_member(const CanonicalValue& value,
                                            std::string_view member_name,
                                            CanonicalValue replacement) {
  CanonicalValue::Object object = value.as_object();
  const auto iterator = std::find_if(
      object.begin(), object.end(), [member_name](const auto& member) {
        return member.first == member_name;
      });
  if (iterator == object.end()) {
    fail("$", "internal assistant codec identity field is absent");
  }
  iterator->second = std::move(replacement);
  return CanonicalValue(std::move(object));
}

// The assistant tool and skill table. Tool descriptions and input schemas
// are built from the loaded node catalog where the mode projection is built
// (Headless); this table carries what Apply qualifies: identity and effect.
struct AssistantToolRow final {
  std::string_view tool_id;
  core::AssistantToolEffect effect;
};

constexpr auto kReadOnly = core::AssistantToolEffect::kReadOnly;
constexpr auto kDraftMutating = core::AssistantToolEffect::kComposerDraftMutating;

constexpr std::array<AssistantToolRow, 30U> kAssistantTools{{
    {"alarms_read", kReadOnly},
    {"blackboard_draft_remove_keys", kDraftMutating},
    {"blackboard_draft_remove_namespaces", kDraftMutating},
    {"blackboard_draft_upsert_keys", kDraftMutating},
    {"blackboard_draft_upsert_namespaces", kDraftMutating},
    {"catalog_read", kReadOnly},
    {"deployment_capabilities_read", kReadOnly},
    {"history_read", kReadOnly},
    {"inspect_isaac_scene", kReadOnly},
    {"program_draft_remove_contact_policies", kDraftMutating},
    {"program_draft_remove_parameters", kDraftMutating},
    {"program_draft_upsert_contact_policies", kDraftMutating},
    {"program_draft_upsert_parameters", kDraftMutating},
    {"program_read", kReadOnly},
    {"program_validate", kReadOnly},
    {"scene_draft_add_object", kDraftMutating},
    {"scene_draft_remove_objects", kDraftMutating},
    {"scene_draft_update_object", kDraftMutating},
    {"scene_draft_upsert_objects", kDraftMutating},
    {"scene_inspect", kReadOnly},
    {"skills_read", kReadOnly},
    {"status_read", kReadOnly},
    {"tree_draft_change_node_kind", kDraftMutating},
    {"tree_draft_delete_node", kDraftMutating},
    {"tree_draft_insert_node", kDraftMutating},
    {"tree_draft_move_node", kDraftMutating},
    {"tree_draft_rename_node", kDraftMutating},
    {"tree_draft_replace_subtree", kDraftMutating},
    {"tree_draft_update_node_params", kDraftMutating},
    {"tree_draft_wrap_node", kDraftMutating},
}};

// The installed skill catalog (manyforge_behavior/resources/skills).
constexpr std::array<std::string_view, 3U> kAssistantSkills{
    "move_to_named_config", "pick_object", "place_object"};

[[nodiscard]] CanonicalValue encode_registry(
    const core::AssistantInstrumentRegistryDTO& registry) {
  CanonicalValue::Array skills;
  skills.reserve(registry.skills.size());
  for (const auto& skill : registry.skills) {
    skills.emplace_back(CanonicalValue::Object{
        {"requires_tools", encode_string_array(skill.requires_tools)},
        {"skill_id", CanonicalValue(skill.skill_id)},
    });
  }

  CanonicalValue::Array tools;
  tools.reserve(registry.tools.size());
  for (const auto& tool : registry.tools) {
    tools.emplace_back(CanonicalValue::Object{
        {"effect", CanonicalValue(std::string(core::to_string(tool.effect)))},
        {"tool_id", CanonicalValue(tool.tool_id)},
    });
  }

  return CanonicalValue(CanonicalValue::Object{
      {"api_version", CanonicalValue(registry.api_version)},
      {"registry_digest", CanonicalValue(registry.registry_digest.value())},
      {"skills", CanonicalValue(std::move(skills))},
      {"tools", CanonicalValue(std::move(tools))},
  });
}

[[nodiscard]] std::pair<core::AssistantInstrumentRegistryDTO, CanonicalValue>
make_registry_value() {
  core::AssistantInstrumentRegistryDTO value{
      std::string(kAssistantInstrumentRegistryApiVersion),
      core::ContentDigest(std::string(kZeroDigest)),
      {},
      {},
  };
  for (const auto skill_id : kAssistantSkills) {
    value.skills.push_back({std::string(skill_id), {}});
  }
  for (const auto& row : kAssistantTools) {
    value.tools.push_back({std::string(row.tool_id), row.effect});
  }
  CanonicalValue normalized = encode_registry(value);
  const std::string digest = detail::compute_content_identity(
      normalized, kInstrumentRegistryIdentitySpec);
  value.registry_digest = core::ContentDigest(digest);
  normalized =
      replace_member(normalized, "registry_digest", CanonicalValue(digest));
  return {std::move(value), std::move(normalized)};
}

// Optional `sampling: {temperature?, top_p?, top_k?}`; at least one field.
[[nodiscard]] core::AssistantModelSamplingDTO parse_model_sampling(
    const CanonicalValue& value, std::string_view path) {
  const auto& object = require_object(value, path);
  require_fields(object, {}, {"temperature", "top_p", "top_k"}, path);
  if (object.empty()) {
    fail(path, "sampling must set at least one field");
  }
  core::AssistantModelSamplingDTO sampling;
  if (const CanonicalValue* temperature = find_member(object, "temperature")) {
    const auto* number = std::get_if<double>(&temperature->storage());
    if (number == nullptr || !std::isfinite(*number) || *number < 0.0 ||
        *number > kMaximumSamplingTemperature) {
      fail(child_path(path, "temperature"),
           "expected a finite number from 0 to 2");
    }
    sampling.temperature = *number;
  }
  if (const CanonicalValue* top_p = find_member(object, "top_p")) {
    sampling.top_p = require_bounded_positive_number(
        *top_p, child_path(path, "top_p"), 1.0);
  }
  if (const CanonicalValue* top_k = find_member(object, "top_k")) {
    sampling.top_k = require_bounded_positive_integer(
        *top_k, child_path(path, "top_k"), kMaximumSamplingTopK);
  }
  return sampling;
}

[[nodiscard]] core::AssistantModelEndpointRequestContractDTO
parse_model_request_contract(const CanonicalValue& value,
                             std::string_view path) {
  const auto& object = require_object(value, path);
  // `reasoning` (on|off, EXPERIMENTAL) and `sampling` are optional.
  require_fields(object,
                 {"default_timeout_s", "max_output_tokens",
                  "supports_response_tools", "supports_json_object",
                  "supports_image_input", "supported_image_transports",
                  "tool_call_family"},
                 {"reasoning", "sampling"}, path);
  const CanonicalValue* reasoning_value = find_member(object, "reasoning");
  std::optional<core::AssistantModelSamplingDTO> sampling;
  if (const CanonicalValue* sampling_value = find_member(object, "sampling")) {
    sampling = parse_model_sampling(*sampling_value,
                                    child_path(path, "sampling"));
  }
  std::optional<bool> reasoning;
  if (reasoning_value != nullptr) {
    const std::string& text =
        require_string(*reasoning_value, child_path(path, "reasoning"));
    if (text != "on" && text != "off") {
      fail(child_path(path, "reasoning"), "reasoning must be on or off");
    }
    reasoning = text == "on";
  }
  const auto& source_transports = require_array(
      require_member(object, "supported_image_transports", path),
      child_path(path, "supported_image_transports"));
  if (source_transports.size() > 2U) {
    fail(child_path(path, "supported_image_transports"),
         "image transport list exceeds the closed v1 registry");
  }
  std::vector<core::AssistantImageTransport> transports;
  transports.reserve(source_transports.size());
  for (std::size_t index = 0U; index < source_transports.size(); ++index) {
    transports.push_back(parse_image_transport(
        source_transports[index],
        index_path(child_path(path, "supported_image_transports"), index)));
  }
  std::sort(transports.begin(), transports.end(),
            [](const auto left, const auto right) {
              return core::to_string(left) < core::to_string(right);
            });
  if (std::adjacent_find(transports.begin(), transports.end()) !=
      transports.end()) {
    fail(child_path(path, "supported_image_transports"),
         "duplicate image transport");
  }
  const bool supports_image_input = require_bool(
      require_member(object, "supports_image_input", path),
      child_path(path, "supports_image_input"));
  if (supports_image_input != !transports.empty()) {
    fail(path,
         "supports_image_input must match the presence of a supported image "
         "transport");
  }
  return {
      require_bounded_positive_number(
          require_member(object, "default_timeout_s", path),
          child_path(path, "default_timeout_s"),
          kMaximumModelRequestTimeoutSeconds),
      require_bounded_positive_integer(
          require_member(object, "max_output_tokens", path),
          child_path(path, "max_output_tokens"), kMaximumModelOutputTokens),
      require_bool(require_member(object, "supports_response_tools", path),
                   child_path(path, "supports_response_tools")),
      require_bool(require_member(object, "supports_json_object", path),
                   child_path(path, "supports_json_object")),
      supports_image_input,
      std::move(transports),
      parse_tool_call_family(require_member(object, "tool_call_family", path),
                             child_path(path, "tool_call_family")),
      reasoning,
      std::move(sampling),
  };
}

[[nodiscard]] core::AssistantModelEndpointAliasDTO parse_model_alias(
    std::string alias, const CanonicalValue& value, std::string_view path) {
  const auto& object = require_object(value, path);
  require_exact_fields(
      object, {"external_resource_alias", "model_id", "request_contract"},
      path);
  return {
      std::move(alias),
      core::ExternalResourceAlias(parse_portable_token(
          require_member(object, "external_resource_alias", path),
          child_path(path, "external_resource_alias"))),
      parse_bounded_text(require_member(object, "model_id", path),
                         child_path(path, "model_id"), false),
      parse_model_request_contract(
          require_member(object, "request_contract", path),
          child_path(path, "request_contract")),
  };
}

[[nodiscard]] core::AssistantModelAuditPolicyDTO parse_audit_policy(const CanonicalValue& value,
                                                                    std::string_view path) {
  const auto& object = require_object(value, path);
  require_exact_fields(
      object, {"store_full_prompts", "store_full_responses", "redact_argument_values"}, path);
  return {
      require_bool(require_member(object, "store_full_prompts", path),
                   child_path(path, "store_full_prompts")),
      require_bool(require_member(object, "store_full_responses", path),
                   child_path(path, "store_full_responses")),
      require_bool(require_member(object, "redact_argument_values", path),
                   child_path(path, "redact_argument_values")),
  };
}

[[nodiscard]] core::AssistantPromptResponseContractDTO
parse_prompt_response_contract(const CanonicalValue& value,
                               std::string_view path) {
  const auto& object = require_object(value, path);
  const auto mode = parse_prompt_response_mode(
      require_member(object, "response_mode", path),
      child_path(path, "response_mode"));
  if (mode == core::AssistantPromptResponseMode::kJsonObject) {
    require_exact_fields(object, {"response_mode"}, path);
    return {mode, std::nullopt, std::nullopt};
  }
  require_exact_fields(
      object, {"response_mode", "tool_name", "tool_description"}, path);
  return {
      mode,
      parse_tool_id(require_member(object, "tool_name", path),
                    child_path(path, "tool_name")),
      parse_bounded_text(require_member(object, "tool_description", path),
                         child_path(path, "tool_description"), false),
  };
}

[[nodiscard]] core::AssistantPromptTemplateDTO parse_prompt_template(std::string prompt_template_id,
                                                                     const CanonicalValue& value,
                                                                     std::string_view path) {
  const auto& object = require_object(value, path);
  require_exact_fields(
      object,
      {"version", "template_text", "variables", "response_contract"},
      path);
  const std::string version =
      parse_portable_token(require_member(object, "version", path), child_path(path, "version"));
  const std::string template_text = parse_prompt_template_text(
      require_member(object, "template_text", path), child_path(path, "template_text"));
  std::vector<core::AssistantPromptTemplateVariableDTO> variables = parse_template_variables(
      require_member(object, "variables", path), child_path(path, "variables"));
  const auto response_contract = parse_prompt_response_contract(
      require_member(object, "response_contract", path),
      child_path(path, "response_contract"));
  const core::ContentDigest digest =
      prompt_template_content_digest(prompt_template_id, version, template_text,
                                     variables, response_contract);
  return {
      std::move(prompt_template_id), version, template_text,
      std::move(variables), response_contract, digest,
  };
}

/// A mode's `runs_in`: a non-empty, sorted set of run states.
[[nodiscard]] std::vector<core::AssistantModeRunState> parse_runs_in(
    const CanonicalValue& value, std::string_view path) {
  const auto& array = require_array(value, path);
  if (array.empty()) {
    fail(path, "a mode runs in at least one cell state");
  }
  std::vector<core::AssistantModeRunState> result;
  for (std::size_t index = 0; index < array.size(); ++index) {
    const auto state = parse_run_state(array[index], index_path(path, index));
    if (!result.empty() && state <= result.back()) {
      fail(path, "run states must be sorted and unique");
    }
    result.push_back(state);
  }
  return result;
}

[[nodiscard]] CanonicalValue encode_runs_in(
    const std::vector<core::AssistantModeRunState>& runs_in) {
  CanonicalValue::Array result;
  for (const auto state : runs_in) {
    result.emplace_back(std::string(core::to_string(state)));
  }
  return CanonicalValue(std::move(result));
}

[[nodiscard]] core::AssistantGrantPolicyDTO parse_grant_policy(
    const CanonicalValue& value, std::string_view path) {
  const auto& object = require_object(value, path);
  require_exact_fields(
      object, {"session_kind", "approval_mode", "review_policy"}, path);
  return {
      parse_nullable_enum<core::AssistantSessionKind>(
          require_member(object, "session_kind", path),
          child_path(path, "session_kind"), parse_session_kind),
      parse_nullable_enum<core::AssistantApprovalMode>(
          require_member(object, "approval_mode", path),
          child_path(path, "approval_mode"), parse_approval_mode),
      parse_review_policy(require_member(object, "review_policy", path),
                          child_path(path, "review_policy")),
  };
}

[[nodiscard]] core::AssistantModeCatalogDTO parse_mode_catalog(
    const CanonicalValue& value, std::string_view path) {
  const auto& object = require_object(value, path);
  require_exact_fields(object, {"skills", "tools"}, path);
  return {
      parse_id_list(require_member(object, "skills", path),
                    child_path(path, "skills"), InstrumentIdKind::kPortable),
      parse_id_list(require_member(object, "tools", path),
                    child_path(path, "tools"), InstrumentIdKind::kTool),
  };
}

[[nodiscard]] core::AssistantModeDTO parse_mode(
    std::string mode_id, const CanonicalValue& value, std::string_view path) {
  const auto& object = require_object(value, path);
  // stuck_loop_recovery is optional in a source and defaults to on;
  // compact_history and constrained_regeneration (EXPERIMENTAL) are
  // optional and default to off.
  require_fields(object,
                 {"enabled", "description", "assistant_component_id",
                  "model_endpoint_alias", "runs_in", "grant_policy", "catalog"},
                 {"stuck_loop_recovery", "compact_history",
                  "constrained_regeneration"},
                 path);
  const CanonicalValue* stuck_loop_recovery =
      find_member(object, "stuck_loop_recovery");
  const CanonicalValue* compact_history =
      find_member(object, "compact_history");
  const CanonicalValue* constrained_regeneration =
      find_member(object, "constrained_regeneration");
  return {
      std::move(mode_id),
      require_bool(require_member(object, "enabled", path),
                   child_path(path, "enabled")),
      parse_bounded_text(require_member(object, "description", path),
                         child_path(path, "description"), false),
      parse_portable_token(
          require_member(object, "assistant_component_id", path),
          child_path(path, "assistant_component_id")),
      parse_portable_token(
          require_member(object, "model_endpoint_alias", path),
          child_path(path, "model_endpoint_alias")),
      parse_runs_in(require_member(object, "runs_in", path),
                    child_path(path, "runs_in")),
      parse_grant_policy(require_member(object, "grant_policy", path),
                         child_path(path, "grant_policy")),
      parse_mode_catalog(require_member(object, "catalog", path),
                         child_path(path, "catalog")),
      stuck_loop_recovery == nullptr
          ? true
          : require_bool(*stuck_loop_recovery,
                         child_path(path, "stuck_loop_recovery")),
      compact_history != nullptr &&
          require_bool(*compact_history, child_path(path, "compact_history")),
      constrained_regeneration != nullptr &&
          require_bool(*constrained_regeneration,
                       child_path(path, "constrained_regeneration")),
  };
}

template <typename DTO, typename Parser>
[[nodiscard]] std::vector<DTO> parse_alias_map(const CanonicalValue& value,
                                               std::string_view path,
                                               Parser parser) {
  const auto& object = require_object(value, path);
  if (object.size() > kMaximumAssistantRows) {
    fail(path, "mapping exceeds the 4096-row ceiling");
  }
  std::vector<DTO> result;
  result.reserve(object.size());
  for (const auto& [key, item] : object) {
    if (!is_portable_token(key)) {
      fail(child_path(path, key), "mapping key is not a portable token");
    }
    result.push_back(parser(key, item, child_path(path, key)));
  }
  std::sort(result.begin(), result.end(), [](const DTO& left, const DTO& right) {
    if constexpr (std::is_same_v<DTO, core::AssistantPromptTemplateDTO>) {
      return left.prompt_template_id < right.prompt_template_id;
    } else if constexpr (std::is_same_v<DTO, core::AssistantModelEndpointAliasDTO>) {
      return left.model_endpoint_alias < right.model_endpoint_alias;
    } else {
      return left.mode_id < right.mode_id;
    }
  });
  return result;
}

[[nodiscard]] CanonicalValue encode_source_settings(
    const core::AssistantSettingsDTO& settings) {
  const CanonicalValue audit_policy(CanonicalValue::Object{
      {"redact_argument_values", CanonicalValue(settings.audit_policy.redact_argument_values)},
      {"store_full_prompts", CanonicalValue(settings.audit_policy.store_full_prompts)},
      {"store_full_responses", CanonicalValue(settings.audit_policy.store_full_responses)},
  });

  CanonicalValue::Object prompt_templates;
  for (const auto& prompt : settings.prompt_templates) {
    prompt_templates.emplace_back(prompt.prompt_template_id,
                                  CanonicalValue(CanonicalValue::Object{
                                      {"response_contract",
                                       encode_prompt_response_contract(
                                           prompt.response_contract)},
                                      {"template_text", CanonicalValue(prompt.template_text)},
                                      {"variables", encode_template_variables(prompt.variables)},
                                      {"version", CanonicalValue(prompt.version)},
                                  }));
  }

  CanonicalValue::Object model_aliases;
  for (const auto& alias : settings.model_endpoint_aliases) {
    model_aliases.emplace_back(
        alias.model_endpoint_alias,
        CanonicalValue(CanonicalValue::Object{
            {"external_resource_alias",
             CanonicalValue(alias.external_resource_alias.value())},
            {"model_id", CanonicalValue(alias.model_id)},
            {"request_contract",
             encode_model_request_contract(alias.request_contract)},
        }));
  }

  CanonicalValue::Object modes;
  for (const auto& mode : settings.modes) {
    const CanonicalValue session_kind =
        mode.grant_policy.session_kind.has_value()
            ? CanonicalValue(std::string(
                  core::to_string(*mode.grant_policy.session_kind)))
            : CanonicalValue(nullptr);
    const CanonicalValue approval_mode =
        mode.grant_policy.approval_mode.has_value()
            ? CanonicalValue(std::string(
                  core::to_string(*mode.grant_policy.approval_mode)))
            : CanonicalValue(nullptr);
    CanonicalValue::Object mode_source{
        {"assistant_component_id", CanonicalValue(mode.assistant_component_id)},
        {"catalog",
         CanonicalValue(CanonicalValue::Object{
             {"skills", encode_string_array(mode.catalog.skill_ids)},
             {"tools", encode_string_array(mode.catalog.tool_ids)},
         })},
        {"description", CanonicalValue(mode.description)},
        {"enabled", CanonicalValue(mode.enabled)},
        {"grant_policy",
         CanonicalValue(CanonicalValue::Object{
             {"approval_mode", approval_mode},
             {"review_policy",
              CanonicalValue(std::string(
                  core::to_string(mode.grant_policy.review_policy)))},
             {"session_kind", session_kind},
         })},
        {"model_endpoint_alias",
         CanonicalValue(mode.model_endpoint_alias)},
        {"runs_in", encode_runs_in(mode.runs_in)},
        {"stuck_loop_recovery", CanonicalValue(mode.stuck_loop_recovery)},
    };
    if (mode.compact_history) {
      // Present only when on, so existing settings keep their encoding.
      mode_source.emplace(mode_source.begin() + 2, "compact_history",
                          CanonicalValue(true));
    }
    if (mode.constrained_regeneration) {
      // Sorted after compact_history; present only when on.
      mode_source.emplace(
          mode_source.begin() + (mode.compact_history ? 3 : 2),
          "constrained_regeneration", CanonicalValue(true));
    }
    modes.emplace_back(mode.mode_id, CanonicalValue(std::move(mode_source)));
  }

  return CanonicalValue(CanonicalValue::Object{
      {"audit_policy", audit_policy},
      {"model_endpoint_aliases", CanonicalValue(std::move(model_aliases))},
      {"modes", CanonicalValue(std::move(modes))},
      {"prompt_templates", CanonicalValue(std::move(prompt_templates))},
  });
}

[[nodiscard]] CanonicalValue encode_projection(
    const core::AssistantSettingsDTO& settings,
    const BuildQualifiedAssistantInstrumentRegistry& registry,
    std::string_view projection_digest) {
  const CanonicalValue audit_policy(CanonicalValue::Object{
      {"redact_argument_values", CanonicalValue(settings.audit_policy.redact_argument_values)},
      {"store_full_prompts", CanonicalValue(settings.audit_policy.store_full_prompts)},
      {"store_full_responses", CanonicalValue(settings.audit_policy.store_full_responses)},
  });
  CanonicalValue::Array prompt_templates;
  prompt_templates.reserve(settings.prompt_templates.size());
  for (const auto& prompt : settings.prompt_templates) {
    prompt_templates.emplace_back(CanonicalValue::Object{
        {"content_digest", CanonicalValue(prompt.content_digest.value())},
        {"prompt_template_id", CanonicalValue(prompt.prompt_template_id)},
        {"response_contract",
         encode_prompt_response_contract(prompt.response_contract)},
        {"template_text", CanonicalValue(prompt.template_text)},
        {"variables", encode_template_variables(prompt.variables)},
        {"version", CanonicalValue(prompt.version)},
    });
  }
  CanonicalValue::Array model_aliases;
  for (const auto& alias : settings.model_endpoint_aliases) {
    model_aliases.emplace_back(CanonicalValue::Object{
        {"external_resource_alias",
         CanonicalValue(alias.external_resource_alias.value())},
        {"model_endpoint_alias", CanonicalValue(alias.model_endpoint_alias)},
        {"model_id", CanonicalValue(alias.model_id)},
        {"request_contract",
         encode_model_request_contract(alias.request_contract)},
    });
  }
  CanonicalValue::Array modes;
  for (const auto& mode : settings.modes) {
    const CanonicalValue session_kind =
        mode.grant_policy.session_kind.has_value()
            ? CanonicalValue(std::string(
                  core::to_string(*mode.grant_policy.session_kind)))
            : CanonicalValue(nullptr);
    const CanonicalValue approval_mode =
        mode.grant_policy.approval_mode.has_value()
            ? CanonicalValue(std::string(
                  core::to_string(*mode.grant_policy.approval_mode)))
            : CanonicalValue(nullptr);
    CanonicalValue::Object retained_mode{
        {"assistant_component_id", CanonicalValue(mode.assistant_component_id)},
        {"catalog",
         CanonicalValue(CanonicalValue::Object{
             {"skills", encode_string_array(mode.catalog.skill_ids)},
             {"tools", encode_string_array(mode.catalog.tool_ids)},
         })},
        {"description", CanonicalValue(mode.description)},
        {"enabled", CanonicalValue(mode.enabled)},
        {"grant_policy",
         CanonicalValue(CanonicalValue::Object{
             {"approval_mode", approval_mode},
             {"review_policy",
              CanonicalValue(std::string(
                  core::to_string(mode.grant_policy.review_policy)))},
             {"session_kind", session_kind},
         })},
        {"mode_id", CanonicalValue(mode.mode_id)},
        {"model_endpoint_alias", CanonicalValue(mode.model_endpoint_alias)},
        {"runs_in", encode_runs_in(mode.runs_in)},
        {"stuck_loop_recovery", CanonicalValue(mode.stuck_loop_recovery)},
    };
    if (mode.compact_history) {
      retained_mode.emplace(retained_mode.begin() + 2, "compact_history",
                            CanonicalValue(true));
    }
    if (mode.constrained_regeneration) {
      retained_mode.emplace(
          retained_mode.begin() + (mode.compact_history ? 3 : 2),
          "constrained_regeneration", CanonicalValue(true));
    }
    modes.emplace_back(std::move(retained_mode));
  }
  return CanonicalValue(CanonicalValue::Object{
      {"api_version",
       CanonicalValue(std::string(kRetainedAssistantProjectionApiVersion))},
      {"audit_policy", audit_policy},
      {"instruments", registry.normalized_root()},
      {"model_endpoint_aliases", CanonicalValue(std::move(model_aliases))},
      {"modes", CanonicalValue(std::move(modes))},
      {"prompt_templates", CanonicalValue(std::move(prompt_templates))},
      {"projection_digest", CanonicalValue(std::string(projection_digest))},
  });
}

template <typename DTO, typename Key>
[[nodiscard]] const DTO* find_by_key(const std::vector<DTO>& rows,
                                     std::string_view key, Key accessor) {
  const auto iterator = std::lower_bound(
      rows.begin(), rows.end(), key,
      [&accessor](const DTO& row, std::string_view candidate) {
        return accessor(row) < candidate;
      });
  return iterator != rows.end() && accessor(*iterator) == key ? &*iterator
                                                              : nullptr;
}

void require_disjoint_registry(
    const core::AssistantInstrumentRegistryDTO& registry) {
  std::set<std::string> ids;
  for (const auto& skill : registry.skills) {
    if (!ids.insert(skill.skill_id).second) {
      fail("$.assistant.instruments", "instrument namespaces overlap");
    }
  }
  for (const auto& tool : registry.tools) {
    if (!ids.insert(tool.tool_id).second) {
      fail("$.assistant.instruments", "instrument namespaces overlap");
    }
  }
}

void require_mode_semantics(
    const core::AssistantModeDTO& mode,
    const core::AssistantInstrumentRegistryDTO& registry) {
  const std::string path = "$.assistant.modes." + mode.mode_id;
  const auto find_tool = [&registry](std::string_view tool_id) {
    return find_by_key(
        registry.tools, tool_id,
        [](const auto& tool) -> const std::string& { return tool.tool_id; });
  };
  const auto find_skill = [&registry](std::string_view skill_id) {
    return find_by_key(
        registry.skills, skill_id,
        [](const auto& skill) -> const std::string& { return skill.skill_id; });
  };

  std::vector<core::AssistantToolEffect> effects;
  for (const auto& tool_id : mode.catalog.tool_ids) {
    const auto* tool = find_tool(tool_id);
    if (tool == nullptr) {
      fail(child_path(path, "catalog.tools"),
           "unresolved tool ID '" + tool_id + "'");
    }
    effects.push_back(tool->effect);
  }
  for (const auto& skill_id : mode.catalog.skill_ids) {
    const auto* skill = find_skill(skill_id);
    if (skill == nullptr) {
      fail(child_path(path, "catalog.skills"),
           "unresolved skill ID '" + skill_id + "'");
    }
    for (const auto& required_tool : skill->requires_tools) {
      if (!std::binary_search(mode.catalog.tool_ids.begin(),
                              mode.catalog.tool_ids.end(), required_tool)) {
        fail(child_path(path, "catalog.skills"),
             "skill '" + skill_id + "' requires absent tool '" +
                 required_tool + "'");
      }
    }
  }

  const auto has_effect = [&effects](core::AssistantToolEffect effect) {
    return std::find(effects.begin(), effects.end(), effect) != effects.end();
  };
  const auto require_exact_grant =
      [&mode, &path](std::optional<core::AssistantSessionKind> session,
                    std::optional<core::AssistantApprovalMode> approval,
                    core::AssistantReviewPolicy review) {
        if (mode.grant_policy.session_kind != session ||
            mode.grant_policy.approval_mode != approval ||
            mode.grant_policy.review_policy != review) {
          fail(child_path(path, "grant_policy"),
               "reserved mode grant/review policy is not exact");
        }
      };

  if (mode.mode_id == "composer-assistant") {
    require_exact_grant(std::nullopt, std::nullopt,
                        core::AssistantReviewPolicy::kRequired);
    if (has_effect(core::AssistantToolEffect::kRuntimeMutating)) {
      fail(path, "composer-assistant cannot contain runtime-mutating tools");
    }
    return;
  }
  if (mode.mode_id == "error-recovery") {
    require_exact_grant(core::AssistantSessionKind::kDelegatedRecovery,
                        core::AssistantApprovalMode::kPolicyGranted,
                        core::AssistantReviewPolicy::kBoundedByRecoveryProfile);
    if (has_effect(core::AssistantToolEffect::kComposerDraftMutating)) {
      fail(path, "error-recovery cannot mutate the Composer draft");
    }
    return;
  }

  const bool has_session = mode.grant_policy.session_kind.has_value();
  const bool has_approval = mode.grant_policy.approval_mode.has_value();
  if (has_session != has_approval) {
    fail(child_path(path, "grant_policy"),
         "session_kind and approval_mode must both be null or both be set");
  }
  if (has_effect(core::AssistantToolEffect::kComposerDraftMutating)) {
    fail(path,
         "Composer Draft mutation is reserved to composer-assistant mode");
  }
  if (!has_session) {
    if (has_effect(core::AssistantToolEffect::kRuntimeMutating) ||
        mode.grant_policy.review_policy ==
            core::AssistantReviewPolicy::kBoundedByRecoveryProfile) {
      fail(child_path(path, "grant_policy"),
           "an ungranted custom mode cannot mutate runtime or use recovery "
           "review semantics");
    }
  } else if (*mode.grant_policy.session_kind ==
                 core::AssistantSessionKind::kManualIntervention &&
             *mode.grant_policy.approval_mode ==
                 core::AssistantApprovalMode::kHumanApproved) {
    if (mode.grant_policy.review_policy !=
        core::AssistantReviewPolicy::kRequired) {
      fail(child_path(path, "grant_policy"),
           "manual human-approved custom modes require review");
    }
  } else if (*mode.grant_policy.session_kind ==
                 core::AssistantSessionKind::kDelegatedRecovery &&
             *mode.grant_policy.approval_mode ==
                 core::AssistantApprovalMode::kPolicyGranted) {
    if (mode.grant_policy.review_policy !=
        core::AssistantReviewPolicy::kBoundedByRecoveryProfile) {
      fail(child_path(path, "grant_policy"),
           "delegated policy-granted custom modes require bounded recovery "
           "review semantics");
    }
  } else {
    fail(child_path(path, "grant_policy"),
         "session/approval pair is inconsistent with the closed grant rules");
  }
  if (mode.grant_policy.review_policy ==
          core::AssistantReviewPolicy::kNotRequired &&
      std::any_of(effects.begin(), effects.end(), [](auto effect) {
        return effect != core::AssistantToolEffect::kReadOnly;
      })) {
    fail(path, "a not-required review policy may expose only read-only tools");
  }
}

[[nodiscard]] CanonicalValue projection_to_source(
    const CanonicalValue& projection) {
  const auto& root = require_object(projection, "$.assistant");
  require_exact_fields(
      root,
      {"api_version", "projection_digest", "audit_policy", "model_endpoint_aliases",
       "modes", "prompt_templates", "instruments"},
      "$.assistant");
  const std::string& api_version =
      require_string(require_member(root, "api_version", "$.assistant"), "$.assistant.api_version");
  if (api_version != kRetainedAssistantProjectionApiVersion) {
    fail("$.assistant.api_version", "unsupported retained assistant version");
  }
  (void)parse_digest(require_member(root, "projection_digest", "$.assistant"),
                     "$.assistant.projection_digest");
  if (!detail::verify_content_identity(projection,
                                       kAssistantProjectionIdentitySpec)) {
    fail("$.assistant.projection_digest",
         "digest does not match the exact retained assistant projection");
  }

  const auto& expected_registry =
      build_qualified_assistant_instrument_registry();
  if (require_member(root, "instruments", "$.assistant") !=
      expected_registry.normalized_root()) {
    fail("$.assistant.instruments",
         "retained registry differs from the build-qualified registry");
  }

  const CanonicalValue& audit_policy = require_member(root, "audit_policy", "$.assistant");
  (void)parse_audit_policy(audit_policy, "$.assistant.audit_policy");

  CanonicalValue::Object prompt_template_map;
  const auto& prompt_templates = require_array(
      require_member(root, "prompt_templates", "$.assistant"), "$.assistant.prompt_templates");
  for (std::size_t index = 0; index < prompt_templates.size(); ++index) {
    const std::string path = index_path("$.assistant.prompt_templates", index);
    const auto& object = require_object(prompt_templates[index], path);
    require_exact_fields(
        object,
        {"prompt_template_id", "version", "template_text", "variables",
         "response_contract", "content_digest"},
        path);
    const std::string prompt_template_id = parse_portable_token(
        require_member(object, "prompt_template_id", path), child_path(path, "prompt_template_id"));
    const core::ContentDigest declared_digest = parse_digest(
        require_member(object, "content_digest", path), child_path(path, "content_digest"));
    CanonicalValue::Object source_template{
        {"response_contract",
         require_member(object, "response_contract", path)},
        {"template_text", require_member(object, "template_text", path)},
        {"variables", require_member(object, "variables", path)},
        {"version", require_member(object, "version", path)},
    };
    const auto parsed =
        parse_prompt_template(prompt_template_id, CanonicalValue(source_template), path);
    if (parsed.content_digest != declared_digest) {
      fail(child_path(path, "content_digest"),
           "does not match the normalized prompt-template content");
    }
    prompt_template_map.emplace_back(prompt_template_id,
                                     CanonicalValue(std::move(source_template)));
  }

  CanonicalValue::Object model_alias_map;
  const auto& model_aliases = require_array(
      require_member(root, "model_endpoint_aliases", "$.assistant"),
      "$.assistant.model_endpoint_aliases");
  for (std::size_t index = 0; index < model_aliases.size(); ++index) {
    const std::string path =
        index_path("$.assistant.model_endpoint_aliases", index);
    const auto& object = require_object(model_aliases[index], path);
    require_exact_fields(object,
                         {"model_endpoint_alias", "external_resource_alias",
                          "model_id", "request_contract"},
                         path);
    const std::string alias = parse_portable_token(
        require_member(object, "model_endpoint_alias", path),
        child_path(path, "model_endpoint_alias"));
    model_alias_map.emplace_back(
        alias,
        CanonicalValue(CanonicalValue::Object{
            {"external_resource_alias",
             require_member(object, "external_resource_alias", path)},
            {"model_id", require_member(object, "model_id", path)},
            {"request_contract",
             require_member(object, "request_contract", path)},
        }));
  }

  CanonicalValue::Object mode_map;
  const auto& modes = require_array(require_member(root, "modes", "$.assistant"),
                                    "$.assistant.modes");
  for (std::size_t index = 0; index < modes.size(); ++index) {
    const std::string path = index_path("$.assistant.modes", index);
    const auto& object = require_object(modes[index], path);
    require_fields(object,
                   {"mode_id", "enabled", "description", "assistant_component_id",
                    "model_endpoint_alias", "runs_in", "grant_policy", "catalog",
                    "stuck_loop_recovery"},
                   {"compact_history", "constrained_regeneration"}, path);
    const std::string mode_id = parse_portable_token(
        require_member(object, "mode_id", path), child_path(path, "mode_id"));
    CanonicalValue::Object source_mode;
    for (const auto& [name, value] : object) {
      if (name != "mode_id") {
        source_mode.emplace_back(name, value);
      }
    }
    mode_map.emplace_back(mode_id, CanonicalValue(std::move(source_mode)));
  }

  return CanonicalValue(CanonicalValue::Object{
      {"audit_policy", audit_policy},
      {"model_endpoint_aliases", CanonicalValue(std::move(model_alias_map))},
      {"modes", CanonicalValue(std::move(mode_map))},
      {"prompt_templates", CanonicalValue(std::move(prompt_template_map))},
  });
}

}  // namespace

bool TypedAssistantSettingsCandidate::source_was_present() const noexcept {
  return data_->source_was_present;
}

bool TypedAssistantSettingsCandidate::enabled() const noexcept {
  return data_->value.enabled();
}

const core::AssistantSettingsDTO& TypedAssistantSettingsCandidate::value()
    const noexcept {
  return data_->value;
}

const CanonicalValue& TypedAssistantSettingsCandidate::normalized_source()
    const noexcept {
  return data_->normalized_source;
}

const core::AssistantInstrumentRegistryDTO&
BuildQualifiedAssistantInstrumentRegistry::value() const noexcept {
  return data_->value;
}

const CanonicalValue&
BuildQualifiedAssistantInstrumentRegistry::normalized_root() const noexcept {
  return data_->normalized_root;
}

const BuildQualifiedAssistantInstrumentRegistry&
build_qualified_assistant_instrument_registry() {
  static const BuildQualifiedAssistantInstrumentRegistry registry = [] {
    auto [value, normalized] = make_registry_value();
    return BuildQualifiedAssistantInstrumentRegistry(
        std::make_shared<
            const BuildQualifiedAssistantInstrumentRegistry::Data>(
            std::move(value), std::move(normalized)));
  }();
  return registry;
}

bool QualifiedAssistantSettings::enabled() const noexcept {
  return data_->enabled;
}

const core::AssistantSettingsDTO& QualifiedAssistantSettings::value()
    const noexcept {
  return data_->value;
}

const core::AssistantInstrumentRegistryDTO&
QualifiedAssistantSettings::instruments() const noexcept {
  return data_->instruments;
}

const CanonicalValue& QualifiedAssistantSettings::normalized_projection()
    const noexcept {
  return data_->normalized_projection;
}

const core::ContentDigest& QualifiedAssistantSettings::projection_digest()
    const noexcept {
  return data_->projection_digest;
}

core::RetainedAssistantProjectionDTO
QualifiedAssistantSettings::retained_projection() const {
  if (!enabled()) {
    throw AssistantSettingsCodecError(
        "disabled assistant settings have no retained projection");
  }
  return {projection_digest(), canonical_json(normalized_projection())};
}

TypedAssistantSettingsCandidate decode_assistant_settings_candidate(
    const ParsedArtifactCandidate& parsed_deployment) {
  const auto& root = require_object(parsed_deployment.root(), "$");
  const CanonicalValue* assistant = find_member(root, "assistant");
  if (assistant == nullptr) {
    return TypedAssistantSettingsCandidate(
        std::make_shared<const TypedAssistantSettingsCandidate::Data>(
            false, core::AssistantSettingsDTO{},
            CanonicalValue(CanonicalValue::Object{})));
  }
  const auto& object = require_object(*assistant, "$.assistant");
  if (object.empty()) {
    return TypedAssistantSettingsCandidate(
        std::make_shared<const TypedAssistantSettingsCandidate::Data>(
            true, core::AssistantSettingsDTO{},
            CanonicalValue(CanonicalValue::Object{})));
  }
  require_exact_fields(
      object,
      {"audit_policy", "prompt_templates", "model_endpoint_aliases", "modes"},
      "$.assistant");

  core::AssistantSettingsDTO value{
      parse_audit_policy(require_member(object, "audit_policy", "$.assistant"),
                         "$.assistant.audit_policy"),
      parse_alias_map<core::AssistantPromptTemplateDTO>(
          require_member(object, "prompt_templates", "$.assistant"), "$.assistant.prompt_templates",
          parse_prompt_template),
      parse_alias_map<core::AssistantModelEndpointAliasDTO>(
          require_member(object, "model_endpoint_aliases", "$.assistant"),
          "$.assistant.model_endpoint_aliases", parse_model_alias),
      parse_alias_map<core::AssistantModeDTO>(
          require_member(object, "modes", "$.assistant"),
          "$.assistant.modes", parse_mode),
  };
  if (value.modes.empty()) {
    if (value.prompt_templates.empty() &&
        value.model_endpoint_aliases.empty()) {
      fail("$.assistant",
           "a non-empty assistant block must configure an interactive mode "
           "or a bounded model-query endpoint and prompt; use the exact "
           "empty object to disable assistant facilities");
    }
    if (value.prompt_templates.empty() ||
        value.model_endpoint_aliases.empty()) {
      fail("$.assistant",
           "model-query-only assistant settings require at least one prompt "
           "template and model endpoint alias");
    }
  }
  return TypedAssistantSettingsCandidate(
      std::make_shared<const TypedAssistantSettingsCandidate::Data>(
          true, value, encode_source_settings(value)));
}

QualifiedAssistantSettings qualify_assistant_settings(
    const TypedAssistantSettingsCandidate& candidate) {
  const auto& registry = build_qualified_assistant_instrument_registry();
  require_disjoint_registry(registry.value());

  if (!candidate.enabled()) {
    return QualifiedAssistantSettings(
        std::make_shared<const QualifiedAssistantSettings::Data>(
            false, candidate.value(), registry.value(),
            CanonicalValue(CanonicalValue::Object{}),
            core::ContentDigest(std::string(kZeroDigest))));
  }

  // Model aliases are served by the Deployment's AI model component; they
  // resolve to no external resource.
  for (const auto& mode : candidate.value().modes) {
    if (find_by_key(
            candidate.value().model_endpoint_aliases,
            mode.model_endpoint_alias,
            [](const auto& alias) -> const std::string& {
              return alias.model_endpoint_alias;
            }) == nullptr) {
      fail("$.assistant.modes." + mode.mode_id + ".model_endpoint_alias",
           "model endpoint alias does not resolve");
    }
    require_mode_semantics(mode, registry.value());
  }

  CanonicalValue projection = encode_projection(
      candidate.value(), registry, kZeroDigest);
  const std::string digest = detail::compute_content_identity(
      projection, kAssistantProjectionIdentitySpec);
  projection = replace_member(projection, "projection_digest",
                              CanonicalValue(digest));
  return QualifiedAssistantSettings(
      std::make_shared<const QualifiedAssistantSettings::Data>(
          true, candidate.value(), registry.value(), std::move(projection),
          core::ContentDigest(digest)));
}

QualifiedAssistantSettings verify_retained_assistant_projection(
    const CanonicalValue& projection) {
  const CanonicalValue source = projection_to_source(projection);
  const CanonicalValue deployment(CanonicalValue::Object{
      {"assistant", source},
  });
  const auto candidate = decode_assistant_settings_candidate(
      parse_strict_json_model_yaml(canonical_json(deployment)));
  QualifiedAssistantSettings qualified =
      qualify_assistant_settings(candidate);
  if (!qualified.enabled() ||
      qualified.normalized_projection() != projection) {
    fail("$.assistant",
         "retained projection differs from independent requalification");
  }
  return qualified;
}

}  // namespace manyforge::deployment
