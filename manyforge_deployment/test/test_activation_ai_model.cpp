#include <gtest/gtest.h>

#include <algorithm>
#include <exception>
#include <filesystem>
#include <manyforge_core/types/typed_error.hpp>
#include <manyforge_deployment/assistant_settings_codec.hpp>
#include <manyforge_deployment/component_spec.hpp>
#include <manyforge_deployment/io_signal_catalog.hpp>
#include <manyforge_deployment/ros2_control_execution_runtime_contract.hpp>
#include <manyforge_deployment/verified_retained_bundle.hpp>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "activation_component_profile_internal.hpp"
#include "activation_deployment_intent_internal.hpp"
#include "prepared_activation_materialization_internal.hpp"

namespace {
namespace d = manyforge::deployment;
namespace c = manyforge::core;
namespace detail = manyforge::deployment::detail;
namespace contract = manyforge::deployment::ros2_control_execution_runtime_contract;
using Value = d::CanonicalValue;
using Object = Value::Object;
using Array = Value::Array;

/// Authored `ai_model_v0` component.
Value AiModel(std::string id, std::string model = "gemma4-12b-it-gguf",
              Value api_key_env = Value(nullptr),
              std::string base_url = "http://127.0.0.1:8000/v1") {
  return Value(Object{
      {"component_id", Value(std::move(id))},
      {"component_profile_id", Value("ai_model_v0")},
      {"target_settings", Value(Object{})},
      {"configuration",
       Value(Object{{"runtime_kind", Value("ai_model")},
                    {"runtime_configuration",
                     Value(Object{{"base_url", Value(std::move(base_url))},
                                  {"model", Value(std::move(model))},
                                  {"api_key_env", std::move(api_key_env)}})}})}});
}

/// Authored assistant component (`manyforge.assistant`).
Value Assistant(std::string model_component_id,
                std::string profile_id = "assistant_scene_authoring",
                std::string id = "assistant") {
  return Value(Object{
      {"component_id", Value(std::move(id))},
      {"component_profile_id", Value("manyforge.assistant")},
      {"target_settings", Value(Object{})},
      {"configuration",
       Value(Object{{"runtime_kind", Value("assistant")},
                    {"runtime_configuration",
                     Value(Object{{"model_component_id", Value(std::move(model_component_id))},
                                  {"assistant_profile_id", Value(std::move(profile_id))}})}})}});
}

Value Intent(Array components,
             std::optional<std::string> assistant_profile_id = std::nullopt,
             Object assistant_model_components = {}) {
  Object root{{"api_version", Value("manyforge.deployment_intent.v2")},
              {"deployment_artifact_id", Value("authored_ai_model")},
              {"name", Value("authored_ai_model")},
              {"compatibility", Value(Object{{"lane", Value("manyforge-only")},
                                             {"provides", Value(Array{})},
                                             {"tags", Value(Array{})}})},
              {"attachments", Value(Array{})},
              {"components", Value(std::move(components))},
              {"policies", Value(Object{})}};
  if (assistant_profile_id)
    root.emplace_back("assistant_profile_id", Value(*assistant_profile_id));
  if (!assistant_model_components.empty())
    root.emplace_back("assistant_model_components",
                      Value(std::move(assistant_model_components)));
  return Value(std::move(root));
}

detail::PreparedActivationMaterialization Materialize(const Value &authored) {
  const auto intent = detail::parse_activation_deployment_intent(authored, {});
  const auto root = std::filesystem::path(MANYFORGE_WORKSPACE_ROOT);
  const auto profiles =
      detail::resolve_activation_component_profiles(root, intent);
  return detail::materialize_prepared_activation(
      root, {}, intent, c::CellId("authored_ai_model_cell"), profiles, false);
}

template <class Call> std::string TypedCode(Call call) {
  try {
    call();
  } catch (const c::TypedError &error) {
    return error.code();
  }
  return {};
}

std::string Refusal(const Value &authored) {
  try {
    (void)Materialize(authored);
  } catch (const std::exception &error) {
    return error.what();
  }
  return {};
}

TEST(ActivationAiModel, ModelFreeComponentCompilesToOneProviderDomain) {
  const auto materialized =
      Materialize(Intent(Array{AiModel("assistant_model", "served-model",
                                       Value("MANYFORGE_AI_MODEL_TOKEN"))}));
  ASSERT_NE(materialized.bundle, nullptr);
  const auto &bundle = *materialized.bundle;
  EXPECT_TRUE(d::verify_retained_bundle_identity(bundle));
  const auto &graph = bundle.value().cell_graph;
  EXPECT_TRUE(graph.model_instances.empty());
  EXPECT_TRUE(graph.control_bindings.empty());
  ASSERT_EQ(graph.controllable_components.size(), 1U);
  EXPECT_TRUE(graph.controllable_components.front()
                  .component_type_id.value()
                  .starts_with("manyforge.ai_model."));
  ASSERT_EQ(graph.control_domains.size(), 1U);
  const auto *provider = graph.control_domains.front().provider();
  ASSERT_NE(provider, nullptr);
  EXPECT_EQ(provider->provider_manifest_id.value(),
            d::kAiModelProviderManifestId);
  EXPECT_EQ(provider->controllable_component_id.value(), "assistant_model");
  EXPECT_EQ(provider->signal_names,
            (std::vector<std::string>{std::string(contract::kAiModelStreamId)}));
  EXPECT_TRUE(graph.control_domains.front()
                  .control_domain_id.value()
                  .starts_with("ai_model.provider."));
  // One ai.model.invoke target binding plus the five lifecycle selectors.
  const auto &providers = bundle.value().capability_provider_bindings;
  EXPECT_EQ(std::count_if(providers.begin(), providers.end(),
                          [](const auto &row) {
                            return row.capability_id.value() ==
                                   d::kAiModelCapabilityId;
                          }),
            1);
  EXPECT_EQ(std::count_if(providers.begin(), providers.end(),
                          [](const auto &row) {
                            return row.provider_manifest_id.value() ==
                                   d::kAiModelProviderManifestId;
                          }),
            6);
  EXPECT_FALSE(bundle.value().assistant.has_value());

  // Runtime unit decode of the `ai_models` row.
  const auto configuration = contract::require_configuration(
      bundle, graph.runtime_units.front().runtime_unit_id);
  ASSERT_EQ(configuration.ai_models.size(), 1U);
  const auto &model = configuration.ai_models.front();
  EXPECT_EQ(model.controllable_component_id.value(), "assistant_model");
  EXPECT_EQ(model.base_url, "http://127.0.0.1:8000/v1");
  EXPECT_EQ(model.model, "served-model");
  EXPECT_EQ(model.api_key_env, std::optional<std::string>("MANYFORGE_AI_MODEL_TOKEN"));
  EXPECT_EQ(contract::find_ai_model(
                configuration, c::ControllableComponentId("assistant_model")),
            &model);
  EXPECT_EQ(contract::find_ai_model(configuration,
                                    c::ControllableComponentId("other")),
            nullptr);
  EXPECT_TRUE(configuration.pose_sources.empty());
  EXPECT_TRUE(configuration.controller_profiles.empty());
}

TEST(ActivationAiModel, NullApiKeyEnvDecodesAsNone) {
  const auto materialized = Materialize(Intent(Array{AiModel("assistant_model")}));
  ASSERT_NE(materialized.bundle, nullptr);
  const auto &graph = materialized.bundle->value().cell_graph;
  const auto configuration = contract::require_configuration(
      *materialized.bundle, graph.runtime_units.front().runtime_unit_id);
  ASSERT_EQ(configuration.ai_models.size(), 1U);
  EXPECT_FALSE(configuration.ai_models.front().api_key_env.has_value());
  const auto rows = d::ResolveRetainedAiModels(graph);
  ASSERT_TRUE(rows.ok());
  ASSERT_EQ(rows.value().size(), 1U);
  EXPECT_EQ(rows.value().front().model, "gemma4-12b-it-gguf");
}

void ExpectRefused(const Value &authored, std::string_view reason) {
  const auto message = Refusal(authored);
  EXPECT_NE(message.find(reason), std::string::npos) << message;
}

TEST(ActivationAiModel, InvalidComponentsAreRefused) {
  const auto refused = ExpectRefused;
  refused(Intent(Array{AiModel("assistant_model", "m", Value(nullptr),
                               "ftp://127.0.0.1/v1")}),
          "ai_model_configuration_invalid");
  refused(Intent(Array{AiModel("assistant_model", "")}),
          "ai_model_configuration_invalid");
  // Only MANYFORGE_AI_* host variables reach the Workbench container.
  refused(Intent(Array{AiModel("assistant_model", "m", Value("OPENAI_API_KEY"))}),
          "ai_model_api_key_env_invalid");
  refused(Intent(Array{AiModel("assistant_model", "m", Value("MANYFORGE_AI_"))}),
          "ai_model_api_key_env_invalid");
  refused(Intent(Array{AiModel("assistant_model", "m",
                               Value("MANYFORGE_AI_lower"))}),
          "ai_model_api_key_env_invalid");
  // Model-free: no physical model fields and the exact profile.
  auto wrong_profile = AiModel("assistant_model").as_object();
  for (auto &[name, value] : wrong_profile)
    if (name == "component_profile_id")
      value = Value("pose_source_v0");
  EXPECT_THROW((void)detail::parse_activation_deployment_intent(
                   Intent(Array{Value(std::move(wrong_profile))}), {}),
               std::invalid_argument);
}

TEST(ActivationAiModel, AssistantComponentCompilesTheChatWithItsAiModel) {
  const auto materialized = Materialize(Intent(
      Array{Assistant("assistant_model"), AiModel("assistant_model", "served-model")}));
  ASSERT_NE(materialized.bundle, nullptr);
  const auto &bundle = *materialized.bundle;
  ASSERT_TRUE(bundle.value().assistant.has_value());
  EXPECT_TRUE(bundle.value().external_resources.empty());
  EXPECT_FALSE(bundle.value().selected_site_overlay_ref.has_value());
  const auto *retained = bundle.normalized_root().find_object_member("assistant");
  ASSERT_NE(retained, nullptr);
  const auto qualified = d::verify_retained_assistant_projection(*retained);
  const auto &settings = qualified.value();
  ASSERT_EQ(settings.model_endpoint_aliases.size(), 1U);
  const auto &alias = settings.model_endpoint_aliases.front();
  EXPECT_EQ(alias.model_endpoint_alias, "assistant_model");
  EXPECT_EQ(alias.external_resource_alias.value(), "assistant_model");
  EXPECT_EQ(alias.model_id, "served-model");
  ASSERT_EQ(settings.modes.size(), 1U);
  EXPECT_EQ(settings.modes.front().mode_id, "composer-assistant");
  EXPECT_EQ(settings.modes.front().model_endpoint_alias, "assistant_model");
  EXPECT_EQ(settings.modes.front().assistant_component_id, "assistant");
  EXPECT_EQ(settings.modes.front().runs_in,
            (std::vector<c::AssistantModeRunState>{
                c::AssistantModeRunState::kNoProgramActive}));
  // The assistant component provides the chat's bridge request.
  const auto &providers = bundle.value().capability_provider_bindings;
  const auto bridge = std::find_if(providers.begin(), providers.end(), [](const auto &row) {
    return row.capability_id.value() == "assistant.bridge.request";
  });
  ASSERT_NE(bridge, providers.end());
  EXPECT_EQ(bridge->provider_manifest_id.value(), "manyforge.provider.assistant_bridge");
  EXPECT_EQ(bridge->scope, c::CapabilityScope::kCellGlobal);
  EXPECT_TRUE(bridge->endpoint_bindings.empty());
}

TEST(ActivationAiModel, ADeploymentLevelChatProfileAloneHasNoChat) {
  // The chat alias is bound by the assistant component only: a Deployment
  // with a chat-only profile and no assistant component retains nothing.
  const auto materialized = Materialize(
      Intent(Array{AiModel("assistant_model")}, "assistant_scene_authoring"));
  ASSERT_NE(materialized.bundle, nullptr);
  EXPECT_FALSE(materialized.bundle->value().assistant.has_value());
}

TEST(ActivationAiModel, OneComponentServesEveryModelAlias) {
  const auto materialized = Materialize(
      Intent(Array{AiModel("assistant_model", "qwen-served")},
             "semantic_shape_selection"));
  ASSERT_NE(materialized.bundle, nullptr);
  const auto *retained =
      materialized.bundle->normalized_root().find_object_member("assistant");
  ASSERT_NE(retained, nullptr);
  const auto qualified = d::verify_retained_assistant_projection(*retained);
  // Its runtime alias only: the chat alias is the assistant component's.
  ASSERT_EQ(qualified.value().model_endpoint_aliases.size(), 1U);
  EXPECT_EQ(qualified.value().model_endpoint_aliases.front().model_endpoint_alias,
            "selection_model");
  EXPECT_EQ(qualified.value().model_endpoint_aliases.front().model_id, "qwen-served");
  EXPECT_TRUE(qualified.value().modes.empty());
}

/// The `assistant.model.query` provider rows of a compiled bundle.
std::vector<c::CapabilityProviderBindingDTO>
ModelQueryRows(const detail::PreparedActivationMaterialization &materialized) {
  std::vector<c::CapabilityProviderBindingDTO> rows;
  for (const auto &row : materialized.bundle->value().capability_provider_bindings)
    if (row.capability_id.value() == "assistant.model.query") rows.push_back(row);
  return rows;
}

// A usable model query (prompt templates and a model endpoint) compiles to
// one cell-global assistant.model.query binding; a profile without prompt
// templates offers none.
TEST(ActivationAiModel, PromptTemplatesCompileToTheModelQueryCapability) {
  const auto with_templates = Materialize(
      Intent(Array{AiModel("assistant_model")}, "semantic_shape_selection"));
  ASSERT_NE(with_templates.bundle, nullptr);
  EXPECT_TRUE(d::verify_retained_bundle_identity(*with_templates.bundle));
  const auto rows = ModelQueryRows(with_templates);
  ASSERT_EQ(rows.size(), 1U);
  const auto &row = rows.front();
  EXPECT_EQ(row.capability_provider_binding_id.value(),
            "authored_ai_model_cell.assistant-model-query.provider");
  EXPECT_EQ(row.scope, c::CapabilityScope::kCellGlobal);
  EXPECT_EQ(row.provider_instance_id.value(),
            "authored_ai_model_cell.assistant_model_query");
  EXPECT_EQ(row.provider_manifest_id.value(), "manyforge.provider.assistant_model_query");
  EXPECT_TRUE(row.endpoint_bindings.empty());

  const auto without_templates = Materialize(
      Intent(Array{AiModel("assistant_model")}, "assistant_scene_authoring"));
  ASSERT_NE(without_templates.bundle, nullptr);
  EXPECT_TRUE(ModelQueryRows(without_templates).empty());
  const auto without_profile = Materialize(Intent(Array{AiModel("assistant_model")}));
  ASSERT_NE(without_profile.bundle, nullptr);
  EXPECT_TRUE(ModelQueryRows(without_profile).empty());
}

TEST(ActivationAiModel, TwoComponentsCompileToTwoDomainsAndStreams) {
  const auto materialized = Materialize(Intent(Array{
      AiModel("chat_model", "chat"), AiModel("selection_model", "select")}));
  ASSERT_NE(materialized.bundle, nullptr);
  const auto &graph = materialized.bundle->value().cell_graph;
  ASSERT_EQ(graph.control_domains.size(), 2U);
  for (const auto &domain : graph.control_domains) {
    ASSERT_NE(domain.provider(), nullptr);
    EXPECT_EQ(domain.provider()->signal_names,
              (std::vector<std::string>{std::string(contract::kAiModelStreamId)}));
  }
  const auto configuration = contract::require_configuration(
      *materialized.bundle, graph.runtime_units.front().runtime_unit_id);
  ASSERT_EQ(configuration.ai_models.size(), 2U);
  EXPECT_EQ(configuration.ai_models[0].controllable_component_id.value(),
            "chat_model");
  EXPECT_EQ(configuration.ai_models[1].model, "select");
}

/// The same component with a lifecycle time budget in its configuration.
Value WithBudget(Value component, Value seconds) {
  auto row = component.as_object();
  for (auto &[name, value] : row) {
    if (name != "configuration") continue;
    auto configuration = value.as_object();
    configuration.emplace_back("lifecycle_command_timeout_s", std::move(seconds));
    value = Value(std::move(configuration));
  }
  return Value(std::move(row));
}

// Per-component time budget: an authored `lifecycle_command_timeout_s`
// compiles to the component's own `lifecycle_command_timeouts` row. Without
// one nothing is added (defaults unchanged).
TEST(ActivationAiModel, LifecycleBudgetCompilesToTheComponentsOwnRow) {
  const auto plain = Materialize(Intent(Array{
      AiModel("chat_model", "chat"), AiModel("selection_model", "select")}));
  ASSERT_NE(plain.bundle, nullptr);
  const auto &plain_graph = plain.bundle->value().cell_graph;
  EXPECT_FALSE(plain_graph.runtime_units.front().configuration.contains(
      "lifecycle_command_timeouts"));
  EXPECT_TRUE(contract::component_lifecycle_budgets_ns(plain_graph).empty());
  // The composition counts each model's default: the one 10 s budget.
  EXPECT_EQ(contract::component_composition_budgets_ns(plain_graph),
            (std::map<std::string, std::int64_t, std::less<>>{
                {"chat_model", 10'000'000'000LL},
                {"selection_model", 10'000'000'000LL}}));

  const auto budgeted = Materialize(Intent(Array{
      WithBudget(AiModel("chat_model", "chat"), Value(120.0)),
      AiModel("selection_model", "select")}));
  ASSERT_NE(budgeted.bundle, nullptr);
  EXPECT_TRUE(d::verify_retained_bundle_identity(*budgeted.bundle));
  const auto &graph = budgeted.bundle->value().cell_graph;
  const auto budgets = contract::component_lifecycle_budgets_ns(graph);
  EXPECT_EQ(budgets, (std::map<std::string, std::int64_t, std::less<>>{
                         {"chat_model", 120'000'000'000LL}}));
  EXPECT_EQ(contract::component_composition_budgets_ns(graph),
            (std::map<std::string, std::int64_t, std::less<>>{
                {"chat_model", 120'000'000'000LL},
                {"selection_model", 10'000'000'000LL}}));
  // A component's identity covers only its own budget row; another's budget
  // leaves it as without any.
  const auto unit = graph.runtime_units.front().runtime_unit_id;
  const auto own = contract::component_configuration_projection(
      *budgeted.bundle, unit, c::ControllableComponentId("chat_model"));
  const auto other = contract::component_configuration_projection(
      *budgeted.bundle, unit, c::ControllableComponentId("selection_model"));
  ASSERT_NE(own.find_object_member("lifecycle_command_timeouts"), nullptr);
  EXPECT_EQ(own.find_object_member("lifecycle_command_timeouts")->as_array().size(), 1U);
  EXPECT_EQ(other, contract::component_configuration_projection(
                       *plain.bundle, plain_graph.runtime_units.front().runtime_unit_id,
                       c::ControllableComponentId("selection_model")));

  for (const auto &seconds : {Value(0.0), Value(3601.0), Value("120")})
    EXPECT_THROW((void)detail::parse_activation_deployment_intent(
                     Intent(Array{WithBudget(AiModel("chat_model"), seconds)}), {}),
                 std::invalid_argument);
}

TEST(ActivationAiModel, MappingSelectsTheComponentOfEachAlias) {
  // The chat alias resolves to the assistant row's model; the runtime aliases
  // through `assistant_model_components`.
  const auto materialized = Materialize(Intent(
      Array{Assistant("chat_model"), AiModel("chat_model", "cosmos"),
            AiModel("selection_model", "qwen")},
      "semantic_shape_selection",
      Object{{"selection_model", Value("selection_model")}}));
  ASSERT_NE(materialized.bundle, nullptr);
  const auto *retained =
      materialized.bundle->normalized_root().find_object_member("assistant");
  ASSERT_NE(retained, nullptr);
  const auto qualified = d::verify_retained_assistant_projection(*retained);
  const auto &aliases = qualified.value().model_endpoint_aliases;
  ASSERT_EQ(aliases.size(), 2U);
  EXPECT_EQ(aliases[0].model_endpoint_alias, "assistant_model");
  EXPECT_EQ(aliases[0].external_resource_alias.value(), "chat_model");
  EXPECT_EQ(aliases[0].model_id, "cosmos");
  EXPECT_EQ(aliases[1].model_endpoint_alias, "selection_model");
  EXPECT_EQ(aliases[1].external_resource_alias.value(), "selection_model");
  EXPECT_EQ(aliases[1].model_id, "qwen");
}

TEST(ActivationAiModel, UnresolvedAliasComponentsAreRefused) {
  const Array two{AiModel("chat_model"), AiModel("selection_model")};
  // Two components and no mapping entry for an alias.
  ExpectRefused(Intent(two, "semantic_shape_selection"),
                "assistant_model_component_unresolved");
  // The chat alias is bound by the assistant component, never mapped.
  ExpectRefused(Intent(two, "semantic_shape_selection",
                       Object{{"assistant_model", Value("chat_model")},
                              {"selection_model", Value("selection_model")}}),
                "assistant_model_component_unresolved");
  // The mapping names a component that is not an AI model component.
  ExpectRefused(Intent(Array{AiModel("selection_model")},
                       "semantic_shape_selection",
                       Object{{"selection_model", Value("other_model")}}),
                "assistant_model_component_unresolved");
  // The mapping key is not an alias of the profile.
  ExpectRefused(Intent(Array{AiModel("assistant_model")},
                       "assistant_scene_authoring",
                       Object{{"vision_model", Value("assistant_model")}}),
                "assistant_model_component_unresolved");
  // A mapping without an assistant profile is malformed.
  EXPECT_THROW((void)detail::parse_activation_deployment_intent(
                   Intent(Array{AiModel("assistant_model")}, std::nullopt,
                          Object{{"assistant_model", Value("assistant_model")}}),
                   {}),
               std::invalid_argument);
}

TEST(ActivationAiModel, AssistantComponentRefusals) {
  // Its model reference names no AI model component.
  ExpectRefused(Intent(Array{Assistant("other_model"), AiModel("chat_model")}),
                "assistant_model_component_unresolved");
  // One assistant per cell.
  EXPECT_EQ(TypedCode([] {
              (void)Materialize(Intent(Array{Assistant("chat_model"), AiModel("chat_model"),
                                             Assistant("chat_model", "assistant_scene_authoring",
                                                       "second_assistant")}));
            }),
            "assistant_component_duplicate");
  // Its profile has no chat mode.
  EXPECT_EQ(TypedCode([] {
              (void)Materialize(
                  Intent(Array{Assistant("chat_model", "vision_bin_selection"), AiModel("chat_model")}));
            }),
            "assistant_profile_has_no_chat_mode");
  // Its configuration is exactly the model and the profile.
  auto extra = Assistant("chat_model").as_object();
  for (auto &[name, value] : extra) {
    if (name != "configuration") continue;
    auto configuration = value.as_object();
    for (auto &[field, settings] : configuration)
      if (field == "runtime_configuration") {
        auto row = settings.as_object();
        row.emplace_back("port", Value(8100.0));
        settings = Value(std::move(row));
      }
    value = Value(std::move(configuration));
  }
  EXPECT_FALSE(Refusal(Intent(Array{Value(std::move(extra)), AiModel("chat_model")})).empty());
}

// The assistant row and its model's endpoint are its spec: a profile change
// or a changed model URL replaces it, an unrelated edit retains it.
TEST(ActivationAiModel, AssistantSpecCoversItsProfileAndItsModelsEndpoint) {
  const auto spec = [](Array components) {
    const auto materialized = Materialize(Intent(std::move(components)));
    return d::component_spec_fingerprint(d::component_spec(
        *materialized.bundle, d::SiteFacts{}, c::ControllableComponentId("assistant")));
  };
  const auto base = spec(Array{Assistant("chat_model"), AiModel("chat_model")});
  EXPECT_NE(base, spec(Array{Assistant("chat_model", "semantic_shape_selection"),
                             AiModel("chat_model")}));
  EXPECT_NE(base, spec(Array{Assistant("chat_model"),
                             AiModel("chat_model", "gemma4-12b-it-gguf", Value(nullptr),
                                     "http://127.0.0.1:8001/v1")}));
  EXPECT_EQ(base, spec(Array{Assistant("chat_model"), AiModel("chat_model"),
                             AiModel("other_model")}));
}

TEST(ActivationAiModel, AssistantProfileWithoutAiModelIsRefused) {
  const auto message =
      Refusal(Intent(Array{}, "assistant_scene_authoring"));
  EXPECT_NE(message.find("assistant_ai_model_component_missing"),
            std::string::npos)
      << message;
}

} // namespace
