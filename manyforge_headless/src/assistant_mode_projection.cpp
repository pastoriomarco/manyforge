#include "manyforge_headless/assistant_mode_projection.hpp"

#include <algorithm>
#include <exception>
#include <manyforge_content_identity/content_identity.hpp>
#include <manyforge_core/types/assistant.hpp>
#include <manyforge_deployment/artifact_codec.hpp>
#include <manyforge_deployment/assistant_settings_codec.hpp>
#include <manyforge_deployment/io_signal_catalog.hpp>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace manyforge::headless {
namespace {

namespace content_identity = manyforge::content_identity;
namespace core = manyforge::core;
namespace deployment = manyforge::deployment;

using content_identity::CanonicalValue;

[[noreturn]] void Fail(const std::string& code, const std::string& detail) {
  throw AssistantModeProjectionError(code, "assistant mode projection: " + detail);
}

template <typename DTO, typename Accessor>
const DTO* Find(const std::vector<DTO>& rows, const std::string_view key, Accessor accessor) {
  const auto iterator = std::lower_bound(
      rows.begin(), rows.end(), key, [&accessor](const DTO& row, const std::string_view candidate) {
        return accessor(row) < candidate;
      });
  return iterator != rows.end() && accessor(*iterator) == key ? &*iterator : nullptr;
}

CanonicalValue TextArray(const std::vector<std::string>& values) {
  CanonicalValue::Array result;
  result.reserve(values.size());
  for (const auto& value : values) {
    result.emplace_back(value);
  }
  return CanonicalValue(std::move(result));
}

CanonicalValue GrantPolicy(const core::AssistantGrantPolicyDTO& policy) {
  return CanonicalValue(CanonicalValue::Object{
      {"approval_mode", policy.approval_mode.has_value()
                            ? CanonicalValue(std::string(core::to_string(*policy.approval_mode)))
                            : CanonicalValue(nullptr)},
      {"review_policy", CanonicalValue(std::string(core::to_string(policy.review_policy)))},
      {"session_kind", policy.session_kind.has_value()
                           ? CanonicalValue(std::string(core::to_string(*policy.session_kind)))
                           : CanonicalValue(nullptr)},
  });
}

CanonicalValue ModelRequestContract(
    const core::AssistantModelEndpointRequestContractDTO& contract) {
  CanonicalValue::Array transports;
  transports.reserve(contract.supported_image_transports.size());
  for (const auto transport : contract.supported_image_transports) {
    transports.emplace_back(std::string(core::to_string(transport)));
  }
  CanonicalValue::Object result{
      {"default_timeout_s", CanonicalValue(contract.default_timeout_s)},
      {"max_output_tokens", CanonicalValue(static_cast<double>(contract.max_output_tokens))},
      {"supported_image_transports", CanonicalValue(std::move(transports))},
      {"supports_image_input", CanonicalValue(contract.supports_image_input)},
      {"supports_json_object", CanonicalValue(contract.supports_json_object)},
      {"supports_response_tools", CanonicalValue(contract.supports_response_tools)},
      {"tool_call_family",
       CanonicalValue(std::string(core::to_string(contract.tool_call_family)))},
  };
  if (contract.reasoning.has_value()) {
    // EXPERIMENTAL: the model server's reasoning (thinking) switch. The
    // object constructor puts members in canonical order.
    result.emplace_back("reasoning",
                        CanonicalValue(std::string(*contract.reasoning ? "on" : "off")));
  }
  if (contract.sampling.has_value()) {
    // Only the set fields; the bridge sends exactly these upstream.
    CanonicalValue::Object sampling;
    if (contract.sampling->temperature.has_value()) {
      sampling.emplace_back("temperature", CanonicalValue(*contract.sampling->temperature));
    }
    if (contract.sampling->top_k.has_value()) {
      sampling.emplace_back("top_k", CanonicalValue(static_cast<double>(*contract.sampling->top_k)));
    }
    if (contract.sampling->top_p.has_value()) {
      sampling.emplace_back("top_p", CanonicalValue(*contract.sampling->top_p));
    }
    result.emplace_back("sampling", CanonicalValue(std::move(sampling)));
  }
  return CanonicalValue(std::move(result));
}

/// The verified assistant settings, or none when the bundle has none.
std::optional<deployment::QualifiedAssistantSettings> VerifyAssistantSettings(
    const deployment::RetainedBundle& active_bundle) {
  const auto& bundle = active_bundle.value();
  const auto* retained_assistant = active_bundle.normalized_root().find_object_member("assistant");
  if (retained_assistant == nullptr || !bundle.assistant.has_value()) {
    return std::nullopt;
  }
  try {
    return deployment::verify_retained_assistant_projection(*retained_assistant);
  } catch (const deployment::AssistantSettingsCodecError& error) {
    Fail("assistant_mode_projection_invalid",
         "retained assistant settings failed independent verification: " +
             std::string(error.what()));
  }
}

/// Whether the bundle has an assistant component: it provides the chat's
/// `assistant.bridge.request`.
bool HasAssistantComponent(const deployment::RetainedBundle& active_bundle) {
  const auto& bindings = active_bundle.value().capability_provider_bindings;
  return std::any_of(bindings.begin(), bindings.end(), [](const auto& binding) {
    return binding.capability_id.value() == "assistant.bridge.request";
  });
}

CanonicalValue RunsIn(const core::AssistantModeDTO& mode) {
  CanonicalValue::Array result;
  for (const auto state : mode.runs_in) {
    result.emplace_back(std::string(core::to_string(state)));
  }
  return CanonicalValue(std::move(result));
}

/// The AI model component serving one model alias: the row named by the
/// alias's `external_resource_alias`, or none.
std::optional<deployment::RetainedAiModelConfiguration> AiModel(
    const deployment::RetainedBundle& active_bundle,
    const core::AssistantModelEndpointAliasDTO& alias) {
  const auto models = deployment::ResolveRetainedAiModels(active_bundle.value().cell_graph);
  if (!models.ok()) {
    Fail("assistant_mode_projection_invalid", "retained AI model rows are invalid");
  }
  for (const auto& row : models.value()) {
    if (row.controllable_component_id.value() == alias.external_resource_alias.value()) {
      return row;
    }
  }
  return std::nullopt;
}

}  // namespace

CanonicalValue ProjectAssistantMode(const deployment::RetainedBundle& active_bundle,
                                    const std::string_view mode_id) {
  const auto& bundle = active_bundle.value();
  const auto verified = VerifyAssistantSettings(active_bundle);
  if (!verified.has_value() || !HasAssistantComponent(active_bundle)) {
    Fail("assistant_component_absent", "the selected bundle has no assistant component");
  }
  const auto& qualified = *verified;
  const auto& settings = qualified.value();
  const auto* mode = Find(settings.modes, mode_id,
                          [](const auto& row) -> const std::string& { return row.mode_id; });
  if (mode == nullptr) {
    Fail("assistant_mode_not_found", "the requested mode is absent from the selected bundle");
  }
  if (!mode->enabled) {
    Fail("assistant_mode_projection_unavailable",
         "the requested mode is disabled in the selected bundle");
  }

  const auto* model =
      Find(settings.model_endpoint_aliases, mode->model_endpoint_alias,
           [](const auto& row) -> const std::string& { return row.model_endpoint_alias; });
  if (model == nullptr) {
    Fail("assistant_mode_projection_invalid",
         "verified mode aliases are not closed over retained settings");
  }
  const auto ai_model = AiModel(active_bundle, *model);
  if (!ai_model.has_value()) {
    Fail("assistant_ai_model_absent", "the selected bundle has no AI model component");
  }

  const auto& registry = qualified.instruments();
  CanonicalValue::Array tools;
  tools.reserve(mode->catalog.tool_ids.size());
  std::set<std::string> visible_tool_ids;
  for (const auto& tool_id : mode->catalog.tool_ids) {
    const auto* tool = Find(registry.tools, tool_id,
                            [](const auto& row) -> const std::string& { return row.tool_id; });
    if (tool == nullptr) {
      Fail("assistant_mode_projection_invalid", "verified mode names an absent tool");
    }
    visible_tool_ids.insert(tool_id);
    tools.emplace_back(CanonicalValue::Object{
        {"effect", CanonicalValue(std::string(core::to_string(tool->effect)))},
        {"tool_id", CanonicalValue(tool->tool_id)},
    });
  }

  CanonicalValue::Array skills;
  skills.reserve(mode->catalog.skill_ids.size());
  for (const auto& skill_id : mode->catalog.skill_ids) {
    const auto* skill = Find(registry.skills, skill_id,
                             [](const auto& row) -> const std::string& { return row.skill_id; });
    if (skill == nullptr) {
      Fail("assistant_mode_projection_invalid", "verified mode names an absent skill");
    }
    if (!std::all_of(
            skill->requires_tools.begin(), skill->requires_tools.end(),
            [&visible_tool_ids](const auto& tool_id) {
              return visible_tool_ids.contains(tool_id);
            })) {
      continue;
    }
    skills.emplace_back(CanonicalValue::Object{
        {"requires_tools", TextArray(skill->requires_tools)},
        {"skill_id", CanonicalValue(skill->skill_id)},
    });
  }

  const auto& environment = bundle.catalogs.node_environment;

  CanonicalValue result(CanonicalValue::Object{
      {"assistant_component_id", CanonicalValue(mode->assistant_component_id)},
      {"assistant_projection_digest", CanonicalValue(qualified.projection_digest().value())},
      {"compact_history", CanonicalValue(mode->compact_history)},
      {"constrained_regeneration", CanonicalValue(mode->constrained_regeneration)},
      {"description", CanonicalValue(mode->description)},
      {"grant_policy", GrantPolicy(mode->grant_policy)},
      {"graph_revision", CanonicalValue(bundle.cell_graph.graph_revision.value())},
      {"mode", CanonicalValue(mode->mode_id)},
      {"model_endpoint",
       CanonicalValue(CanonicalValue::Object{
           {"ai_model_component_id",
            CanonicalValue(ai_model->controllable_component_id.value())},
           {"api_key_env", ai_model->api_key_env.has_value()
                               ? CanonicalValue(*ai_model->api_key_env)
                               : CanonicalValue(nullptr)},
           {"base_url", CanonicalValue(ai_model->base_url)},
           {"model_endpoint_alias", CanonicalValue(model->model_endpoint_alias)},
           {"model_id", CanonicalValue(model->model_id)},
           {"request_contract", ModelRequestContract(model->request_contract)},
       })},
      {"node_environment",
       CanonicalValue(CanonicalValue::Object{
           {"catalog_digest", CanonicalValue(environment.catalog_digest.value())},
           {"catalog_id", CanonicalValue(environment.catalog_id)},
           {"content_version", CanonicalValue(environment.content_version)},
           {"environment_digest", CanonicalValue(environment.environment_digest.value())},
       })},
      {"retained_bundle_id", CanonicalValue(active_bundle.retained_bundle_id().value())},
      {"runs_in", RunsIn(*mode)},
      {"schema_version", CanonicalValue(std::string(kAssistantModeProjectionApiVersion))},
      {"skills", CanonicalValue(std::move(skills))},
      {"stuck_loop_recovery", CanonicalValue(mode->stuck_loop_recovery)},
      {"tools", CanonicalValue(std::move(tools))},
  });
  if (content_identity::canonical_json(result).size() > kMaximumAssistantModeProjectionBytes) {
    Fail("assistant_mode_projection_invalid", "projection exceeds its public byte bound");
  }
  return result;
}

CanonicalValue ProjectAssistantModeIndex(const deployment::RetainedBundle& active_bundle) {
  const auto& bundle = active_bundle.value();
  const auto verified = VerifyAssistantSettings(active_bundle);
  // Without an assistant component the index has no mode.
  const bool chat = verified.has_value() && HasAssistantComponent(active_bundle);

  CanonicalValue::Array modes;
  std::optional<std::string> assistant_component_id;
  if (chat) {
    for (const auto& mode : verified->value().modes) {
      if (!mode.enabled) {
        continue;
      }
      assistant_component_id = mode.assistant_component_id;
      modes.emplace_back(CanonicalValue::Object{
          {"description", CanonicalValue(mode.description)},
          {"grant_policy", GrantPolicy(mode.grant_policy)},
          {"mode", CanonicalValue(mode.mode_id)},
          {"runs_in", RunsIn(mode)},
      });
    }
  }

  const auto& environment = bundle.catalogs.node_environment;
  // The chat alias's component, when the bundle retains it.
  const auto* chat_alias =
      verified.has_value()
          ? Find(verified->value().model_endpoint_aliases, "assistant_model",
                 [](const auto& row) -> const std::string& { return row.model_endpoint_alias; })
          : nullptr;
  const auto ai_model = chat_alias == nullptr
                            ? std::optional<deployment::RetainedAiModelConfiguration>{}
                            : AiModel(active_bundle, *chat_alias);
  CanonicalValue result(CanonicalValue::Object{
      {"ai_model_component_id",
       ai_model.has_value() ? CanonicalValue(ai_model->controllable_component_id.value())
                            : CanonicalValue(nullptr)},
      {"assistant_component_id", assistant_component_id.has_value()
                                     ? CanonicalValue(*assistant_component_id)
                                     : CanonicalValue(nullptr)},
      {"assistant_projection_digest",
       verified.has_value() ? CanonicalValue(verified->projection_digest().value())
                            : CanonicalValue(nullptr)},
      {"graph_revision", CanonicalValue(bundle.cell_graph.graph_revision.value())},
      {"modes", CanonicalValue(std::move(modes))},
      {"node_environment",
       CanonicalValue(CanonicalValue::Object{
           {"catalog_digest", CanonicalValue(environment.catalog_digest.value())},
           {"catalog_id", CanonicalValue(environment.catalog_id)},
           {"content_version", CanonicalValue(environment.content_version)},
           {"environment_digest", CanonicalValue(environment.environment_digest.value())},
       })},
      {"retained_bundle_id", CanonicalValue(active_bundle.retained_bundle_id().value())},
      {"schema_version", CanonicalValue(std::string(kAssistantModeIndexApiVersion))},
  });
  if (content_identity::canonical_json(result).size() > kMaximumAssistantModeIndexBytes) {
    Fail("assistant_mode_projection_invalid", "mode index exceeds its public byte bound");
  }
  return result;
}

}  // namespace manyforge::headless
