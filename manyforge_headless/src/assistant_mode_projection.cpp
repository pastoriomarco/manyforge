#include "manyforge_headless/assistant_mode_projection.hpp"

#include <algorithm>
#include <exception>
#include <manyforge_content_identity/content_identity.hpp>
#include <manyforge_core/types/assistant.hpp>
#include <manyforge_deployment/artifact_codec.hpp>
#include <manyforge_deployment/assistant_settings_codec.hpp>
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
  return CanonicalValue(CanonicalValue::Object{
      {"default_timeout_s", CanonicalValue(contract.default_timeout_s)},
      {"max_output_tokens", CanonicalValue(static_cast<double>(contract.max_output_tokens))},
      {"supported_image_transports", CanonicalValue(std::move(transports))},
      {"supports_image_input", CanonicalValue(contract.supports_image_input)},
      {"supports_json_object", CanonicalValue(contract.supports_json_object)},
      {"supports_response_tools", CanonicalValue(contract.supports_response_tools)},
      {"tool_call_family",
       CanonicalValue(std::string(core::to_string(contract.tool_call_family)))},
  });
}

CanonicalValue InputSchema(const core::AssistantToolDescriptorDTO& tool) {
  try {
    return deployment::parse_strict_json_model_yaml(tool.canonical_input_schema).root();
  } catch (const std::exception& error) {
    Fail("assistant_mode_projection_invalid",
         "build-qualified input schema failed strict decoding for " + tool.tool_id + ": " +
             error.what());
  }
}

deployment::QualifiedAssistantSettings VerifyAssistantSettings(
    const deployment::RetainedBundle& active_bundle) {
  const auto& bundle = active_bundle.value();
  const auto* retained_assistant = active_bundle.normalized_root().find_object_member("assistant");
  if (retained_assistant == nullptr || !bundle.assistant.has_value()) {
    Fail("assistant_mode_projection_unavailable",
         "the selected bundle has no assistant projection");
  }
  try {
    return deployment::verify_retained_assistant_projection(*retained_assistant,
                                                            bundle.external_resources);
  } catch (const deployment::AssistantSettingsCodecError& error) {
    Fail("assistant_mode_projection_invalid",
         "retained assistant settings failed independent verification: " +
             std::string(error.what()));
  }
}

}  // namespace

CanonicalValue ProjectAssistantMode(const deployment::RetainedBundle& active_bundle,
                                    const std::string_view mode_id) {
  const auto& bundle = active_bundle.value();
  const auto qualified = VerifyAssistantSettings(active_bundle);
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

  const auto* provider =
      Find(settings.provider_aliases, mode->provider_alias,
           [](const auto& row) -> const std::string& { return row.provider_alias; });
  const auto* model =
      Find(settings.model_endpoint_aliases, mode->model_endpoint_alias,
           [](const auto& row) -> const std::string& { return row.model_endpoint_alias; });
  if (provider == nullptr || model == nullptr) {
    Fail("assistant_mode_projection_invalid",
         "verified mode aliases are not closed over retained settings");
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
        {"description", CanonicalValue(tool->description)},
        {"effect", CanonicalValue(std::string(core::to_string(tool->effect)))},
        {"input_schema", InputSchema(*tool)},
        {"input_schema_hash", CanonicalValue(tool->input_schema_digest.value())},
        {"tool_id", CanonicalValue(tool->tool_id)},
    });
  }

  CanonicalValue::Array nodes;
  nodes.reserve(mode->catalog.node_ids.size());
  for (const auto& node_id : mode->catalog.node_ids) {
    const auto* node = Find(registry.nodes, node_id,
                            [](const auto& row) -> const std::string& { return row.node_id; });
    if (node == nullptr) {
      Fail("assistant_mode_projection_invalid", "verified mode names an absent node");
    }
    nodes.emplace_back(CanonicalValue::Object{
        {"node_id", CanonicalValue(node->node_id)},
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

  const auto grant_policy = GrantPolicy(mode->grant_policy);
  const CanonicalValue catalog(CanonicalValue::Object{
      {"grant_policy", grant_policy},
      {"mode", CanonicalValue(mode->mode_id)},
      {"nodes", CanonicalValue(nodes)},
      {"skills", CanonicalValue(skills)},
      {"tools", CanonicalValue(tools)},
  });
  const auto catalog_hash = content_identity::framed_sha256(
      "assistant-mode-catalog", kAssistantModeCatalogApiVersion, catalog);
  const auto& environment = bundle.catalogs.node_environment;

  CanonicalValue result(CanonicalValue::Object{
      {"assistant_projection_digest", CanonicalValue(qualified.projection_digest().value())},
      {"catalog_hash", CanonicalValue(catalog_hash)},
      {"description", CanonicalValue(mode->description)},
      {"grant_policy", grant_policy},
      {"graph_revision", CanonicalValue(bundle.cell_graph.graph_revision.value())},
      {"instrument_registry_digest", CanonicalValue(registry.registry_digest.value())},
      {"mode", CanonicalValue(mode->mode_id)},
      {"model_endpoint", CanonicalValue(CanonicalValue::Object{
                             {"model_endpoint_alias", CanonicalValue(model->model_endpoint_alias)},
                             {"model_id", CanonicalValue(model->model_id)},
                             {"request_contract",
                              ModelRequestContract(model->request_contract)},
                         })},
      {"node_environment",
       CanonicalValue(CanonicalValue::Object{
           {"catalog_digest", CanonicalValue(environment.catalog_digest.value())},
           {"catalog_id", CanonicalValue(environment.catalog_id)},
           {"content_version", CanonicalValue(environment.content_version)},
           {"environment_digest", CanonicalValue(environment.environment_digest.value())},
       })},
      {"nodes", CanonicalValue(std::move(nodes))},
      {"provider",
       CanonicalValue(CanonicalValue::Object{
           {"provider_alias", CanonicalValue(provider->provider_alias)},
           {"provider_kind", CanonicalValue(std::string(core::to_string(provider->provider_kind)))},
       })},
      {"retained_bundle_id", CanonicalValue(active_bundle.retained_bundle_id().value())},
      {"schema_version", CanonicalValue(std::string(kAssistantModeProjectionApiVersion))},
      {"skills", CanonicalValue(std::move(skills))},
      {"tools", CanonicalValue(std::move(tools))},
  });
  if (content_identity::canonical_json(result).size() > kMaximumAssistantModeProjectionBytes) {
    Fail("assistant_mode_projection_invalid", "projection exceeds its public byte bound");
  }
  return result;
}

CanonicalValue ProjectAssistantModeIndex(const deployment::RetainedBundle& active_bundle) {
  const auto& bundle = active_bundle.value();
  const auto qualified = VerifyAssistantSettings(active_bundle);
  const auto& settings = qualified.value();

  CanonicalValue::Array modes;
  modes.reserve(settings.modes.size());
  for (const auto& mode : settings.modes) {
    if (!mode.enabled) {
      continue;
    }
    modes.emplace_back(CanonicalValue::Object{
        {"description", CanonicalValue(mode.description)},
        {"grant_policy", GrantPolicy(mode.grant_policy)},
        {"mode", CanonicalValue(mode.mode_id)},
    });
  }

  const auto& environment = bundle.catalogs.node_environment;
  CanonicalValue result(CanonicalValue::Object{
      {"assistant_projection_digest", CanonicalValue(qualified.projection_digest().value())},
      {"graph_revision", CanonicalValue(bundle.cell_graph.graph_revision.value())},
      {"instrument_registry_digest",
       CanonicalValue(qualified.instruments().registry_digest.value())},
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
