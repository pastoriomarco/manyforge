#include "activation_pose_source_internal.hpp"

#include <manyforge_content_identity/content_identity.hpp>
#include <manyforge_core/types/capability_definition.hpp>
#include <manyforge_deployment/closed_schema.hpp>
#include <manyforge_deployment/content_identity.hpp>
#include <manyforge_deployment/deployment_applicability_codec.hpp>
#include <manyforge_deployment/io_signal_catalog.hpp>
#include <manyforge_deployment/scene_pose_source_policy_codec.hpp>
#include <algorithm>
#include <array>
#include <map>
#include <set>
#include <stdexcept>
#include <tuple>

#include "content_identity_internal.hpp"

namespace manyforge::deployment::detail {
namespace {
using Value = CanonicalValue;
using Object = Value::Object;
using Array = Value::Array;
constexpr auto kPoseSourceConfigurationSchema =
    "manyforge.value_schema.runtime.pose_source.v2";
/// Policy token of every object link in the expanded row; its stream is
/// `poses/<object_id>`, independent of the apply policy.
constexpr auto kObjectLinkPolicy = "scene";
constexpr auto kPoseCapability = "scene.pose.observe";
constexpr auto kPoseTarget = "poses";

const Value &member(const Value &value, std::string_view name) {
  const auto *result = value.find_object_member(name);
  if (!result)
    throw std::invalid_argument("pose-source configuration missing " +
                                std::string(name));
  return *result;
}

[[nodiscard]] const Value *gripper_target_configuration(
    const ActivationDeploymentComponent &component,
    const std::string_view command_target_id) {
  const auto *configuration =
      component.configuration.find_object_member("runtime_configuration");
  const auto *targets = configuration
                            ? configuration->find_object_member(
                                  "gripper_target_configurations")
                            : nullptr;
  if (targets == nullptr || !targets->is_array())
    return nullptr;
  const Value *selected = nullptr;
  for (const auto &row : targets->as_array()) {
    const auto *target = row.find_object_member("command_target_id");
    if (target == nullptr || !target->is_string() ||
        target->as_string() != command_target_id)
      continue;
    if (selected != nullptr)
      return nullptr;
    selected = &row;
  }
  return selected;
}

[[nodiscard]] std::string unique_gripper_base_link(
    const ActivationPreparedModel &model, const Value &target_configuration) {
  const auto &asset = model.verified_asset.value();
  const auto binding = std::find_if(
      asset.collision_body_role_bindings.begin(),
      asset.collision_body_role_bindings.end(), [](const auto &row) {
        return row.collision_body_role_id.value() == "gripper_collision";
      });
  if (binding == asset.collision_body_role_bindings.end())
    return {};
  const std::set<std::string, std::less<>> collision_links(
      binding->source_link_names.begin(), binding->source_link_names.end());
  const auto *touch_links =
      target_configuration.find_object_member("touch_links");
  if (touch_links == nullptr || !touch_links->is_array())
    return {};
  std::set<std::string, std::less<>> target_gripper_links;
  for (const auto &touch_link : touch_links->as_array()) {
    if (!touch_link.is_string())
      return {};
    if (collision_links.contains(touch_link.as_string()))
      target_gripper_links.insert(touch_link.as_string());
  }
  if (target_gripper_links.empty())
    return {};
  std::vector<std::string> candidates;
  for (const auto &joint : model.verified_asset.source_ir().urdf().joints) {
    if (joint.type == UrdfJointType::kFixed &&
        target_gripper_links.contains(joint.child_link_name) &&
        collision_links.contains(joint.child_link_name) &&
        !collision_links.contains(joint.parent_link_name))
      candidates.push_back(joint.child_link_name);
  }
  if (candidates.size() != 1U)
    return {};
  return candidates.front();
}

[[nodiscard]] bool has_gripper_target(
    const ActivationDeploymentComponent &component) {
  const auto *configuration =
      component.configuration.find_object_member("runtime_configuration");
  const auto *grippers = configuration
                             ? configuration->find_object_member(
                                   "gripper_target_configurations")
                             : nullptr;
  if (grippers == nullptr || !grippers->is_array())
    return false;
  return !grippers->as_array().empty();
}

[[nodiscard]] Value relative_references(
    const Value &configuration, const ActivationDeploymentIntent &intent,
    const std::map<std::string, const ActivationPreparedModel *,
                   std::less<>> &models_by_component) {
  const auto *simulation = configuration.find_object_member("simulation");
  if (simulation == nullptr)
    return Value(nullptr);
  const auto *references =
      simulation->find_object_member("relative_references");
  const auto *legacy = simulation->find_object_member("relative_reference");
  if (references != nullptr && legacy != nullptr)
    throw std::invalid_argument(
        "scene_pose_source_relative_references_conflict");

  Value result(nullptr);
  if (references != nullptr) {
    result = *references;
  } else if (legacy != nullptr) {
    std::vector<std::pair<const ActivationDeploymentComponent *,
                          const Value *>>
        gripper_targets;
    for (const auto &component : intent.components) {
      const auto *configuration =
          component.configuration.find_object_member("runtime_configuration");
      const auto *targets = configuration
                                ? configuration->find_object_member(
                                      "gripper_target_configurations")
                                : nullptr;
      if (targets == nullptr || !targets->is_array())
        continue;
      for (const auto &target : targets->as_array())
        gripper_targets.emplace_back(&component, &target);
    }
    if (gripper_targets.size() != 1U)
      throw std::invalid_argument(
          "scene_pose_source_relative_references_required: set per-robot references");
    const auto &[component, target] = gripper_targets.front();
    const auto &robot = *component;
    const auto &command_target_id =
        member(*target, "command_target_id").as_string();
    const auto model = models_by_component.find(robot.component_id);
    if (model == models_by_component.end())
      throw std::invalid_argument(
          "scene_pose_source_relative_reference_gripper_link_invalid");
    const auto gripper_link =
        unique_gripper_base_link(*model->second, *target);
    if (gripper_link.empty())
      throw std::invalid_argument(
          "scene_pose_source_relative_reference_gripper_link_invalid");
    result = Value(Array{Value(Object{
        {"controllable_component_id", Value(robot.component_id)},
        {"command_target_id", Value(command_target_id)},
        {"gripper_link", Value(gripper_link)},
        {"entity", member(*legacy, "entity")},
        {"link", member(*legacy, "link")},
        {"source_header_frame_id",
         member(*legacy, "source_header_frame_id")},
    })});
  }

  if (std::holds_alternative<std::nullptr_t>(result.storage()))
    return result;
  if (!result.is_array() || result.as_array().empty())
    throw std::invalid_argument(
        "scene_pose_source_relative_reference_gripper_link_invalid");
  std::set<std::pair<std::string, std::string>> seen;
  Array normalized;
  normalized.reserve(result.as_array().size());
  for (const auto &row : result.as_array()) {
    const auto &component_id =
        member(row, "controllable_component_id").as_string();
    const auto &command_target_id =
        member(row, "command_target_id").as_string();
    const auto model = models_by_component.find(component_id);
    const auto component = std::find_if(
        intent.components.begin(), intent.components.end(),
        [&](const auto &candidate) {
          return candidate.component_id == component_id;
        });
    const auto *target = component == intent.components.end()
                             ? nullptr
                             : gripper_target_configuration(*component,
                                                            command_target_id);
    if (model == models_by_component.end() || target == nullptr ||
        !has_gripper_target(*component))
      throw std::invalid_argument(
          "scene_pose_source_relative_reference_gripper_link_invalid");
    const auto gripper_link =
        unique_gripper_base_link(*model->second, *target);
    const auto *stated_link = row.find_object_member("gripper_link");
    if (gripper_link.empty() ||
        (stated_link != nullptr &&
         (!stated_link->is_string() ||
          stated_link->as_string() != gripper_link)) ||
        !seen.emplace(component_id, command_target_id).second)
      throw std::invalid_argument(
          "scene_pose_source_relative_reference_gripper_link_invalid");
    auto normalized_row = row.as_object();
    std::erase_if(normalized_row, [](const auto &field) {
      return field.first == "gripper_link";
    });
    normalized_row.emplace_back("gripper_link", Value(gripper_link));
    normalized.emplace_back(Value(std::move(normalized_row)));
  }
  return Value(std::move(normalized));
}

[[nodiscard]] Value with_relative_references(Value configuration,
                                              const Value &references) {
  if (!configuration.is_object() ||
      std::holds_alternative<std::nullptr_t>(references.storage()))
    return configuration;
  auto root = configuration.as_object();
  for (auto &[name, value] : root) {
    if (name != "simulation")
      continue;
    auto simulation = value.as_object();
    std::erase_if(simulation, [](const auto &field) {
      return field.first == "relative_reference" ||
             field.first == "relative_references";
    });
    simulation.emplace_back("relative_references", references);
    value = Value(std::move(simulation));
    return Value(std::move(root));
  }
  return configuration;
}

Value with(Value value, std::string_view field, Value selected) {
  auto object = value.as_object();
  for (auto &[name, current] : object) {
    if (name == field) {
      current = std::move(selected);
      return Value(std::move(object));
    }
  }
  object.emplace_back(field, std::move(selected));
  return Value(std::move(object));
}

Value normalize_configuration(const Value &value) {
  for (const auto &entry :
       build_qualified_closed_schema_registry().value_schemas()) {
    if (entry.schema_id().value() == kPoseSourceConfigurationSchema)
      return validate_and_normalize_build_qualified_value(entry, value)
          .normalized_value();
  }
  throw std::invalid_argument(
      "pose-source configuration schema is not qualified by this build");
}

Value numbers(const auto &values) {
  Array result;
  for (const auto value : values)
    result.emplace_back(value);
  return Value(std::move(result));
}

/// Joins one authored component (normalized `pose_source.v2`) with the object
/// links naming it and the `component_streams` budget overrides into the
/// expanded unit row `{controllable_component_id, simulation_poll_period_ms,
/// simulation_entities, detection3d_sources, joint_state?}`.
Value expand_pose_source(
    const std::string &component_id, const Value &config,
    const std::vector<core::ScenePoseSourcePolicyDTO> &links,
    const std::vector<core::StreamParameterOverrideDTO> &streams) {
  const auto budget = member(config, "stream_budget_ns").as_number();
  const auto lifetime = [&](const std::string &object_id,
                            std::string_view policy_id) {
    const auto stream = PoseSourceStreamId(object_id, policy_id);
    for (const auto &row : streams)
      if (row.controllable_component_id.value() == component_id &&
          row.stream_id == stream && row.budget_ns)
        return Value(static_cast<double>(*row.budget_ns));
    return Value(budget);
  };
  const auto *simulation = config.find_object_member("simulation");
  const auto *detection = config.find_object_member("detection3d");
  const auto *overlay = config.find_object_member("robot_overlay");
  const auto *relative =
      simulation ? simulation->find_object_member("relative_references")
                 : nullptr;
  const auto entity_row = [&](const std::string &object_id,
                              std::string_view policy_id, bool eligible,
                              const std::string &entity,
                              const std::string &header,
                              const Value &local_position,
                              const Value &local_orientation) {
    Object row{
        {"object_id", Value(object_id)},
        {"policy_id", Value(std::string(policy_id))},
        {"eligible", Value(eligible)},
        {"frame_kind", Value("cell_frame")},
        {"entity", Value(entity)},
        {"source_header_frame_id", Value(header)},
        {"local_position_xyz", local_position},
        {"local_orientation_xyzw", local_orientation},
        {"source_coordinate_frame_id",
         member(*simulation, "source_coordinate_frame_id")},
        {"target_frame_id", member(*simulation, "target_frame_id")},
        {"lifetime_ns", lifetime(object_id, policy_id)}};
    // Object rows carry it whenever the component has one, so a policy edit
    // never changes the observer document; overlay rows never do.
    if (relative && eligible)
      row.emplace_back("relative_references", *relative);
    return Value(std::move(row));
  };
  const auto offset = [](const Value &object, std::string_view name,
                         Value fallback) {
    const auto *found = object.find_object_member(name);
    return found ? *found : fallback;
  };
  const Value zero = numbers(std::array{0.0, 0.0, 0.0});
  const Value identity = numbers(std::array{0.0, 0.0, 0.0, 1.0});
  std::vector<std::pair<std::tuple<std::string, std::string>, Value>> entities;
  std::vector<std::pair<std::string, Value>> detections;
  for (const auto &link : links) {
    if (link.source_id != component_id)
      continue;
    const bool relative_policy =
        link.apply_policy == "continuous_relative_to_attached";
    if (const auto *key = std::get_if<core::ScenePoseSimulationEntityKeyDTO>(
            &link.source_key)) {
      if (!simulation)
        throw std::invalid_argument("scene_pose_source_mode_missing");
      if (relative_policy &&
          (relative == nullptr || !relative->is_array() ||
           relative->as_array().empty()))
        throw std::invalid_argument(
            "scene_pose_source_relative_reference_missing");
      entities.emplace_back(
          std::tuple{link.scene_resource_id, std::string(kObjectLinkPolicy)},
          entity_row(link.scene_resource_id, kObjectLinkPolicy, true,
                     key->entity, key->source_header_frame_id,
                     numbers(key->local_position_xyz),
                     numbers(key->local_orientation_xyzw)));
      continue;
    }
    const auto &selector =
        std::get<core::ScenePoseDetection3dKeyDTO>(link.source_key);
    if (!detection)
      throw std::invalid_argument("scene_pose_source_mode_missing");
    if (relative_policy)
      throw std::invalid_argument(
          "scene_pose_source_relative_requires_simulation");
    detections.emplace_back(
        link.scene_resource_id,
        Value(Object{
            {"object_id", Value(link.scene_resource_id)},
            {"policy_id", Value(kObjectLinkPolicy)},
            {"eligible", Value(true)},
            {"frame_kind", Value("cell_frame")},
            {"selector", *EncodeScenePoseSourceKey(selector)
                              .find_object_member("detection3d")},
            {"topic", member(*detection, "topic")},
            {"target_frame_id", member(*detection, "target_frame_id")},
            {"fallback_frame_id", member(*detection, "fallback_frame_id")},
            {"lifetime_ns",
             lifetime(link.scene_resource_id, kObjectLinkPolicy)}}));
  }
  Object row{{"controllable_component_id", Value(component_id)},
             {"simulation_poll_period_ms",
              simulation ? member(*simulation, "poll_period_ms") : Value(250.0)}};
  if (overlay) {
    const auto &robot = member(*overlay, "robot_component_id").as_string();
    if (const auto *joint = overlay->find_object_member("joint_state"))
      row.emplace_back(
          "joint_state",
          Value(Object{
              {"object_id", Value(robot)},
              {"policy_id", Value("viewer.measured_robot")},
              {"eligible", Value(false)},
              {"topic", member(*joint, "topic")},
              {"required_joint_names", member(*joint, "required_joint_names")},
              {"lifetime_ns", lifetime(robot, "viewer.measured_robot")}}));
    if (const auto *tcp = overlay->find_object_member("tcp_entity")) {
      if (!simulation)
        throw std::invalid_argument("scene_pose_source_mode_missing");
      entities.emplace_back(
          std::tuple{robot, std::string("viewer.measured_tcp")},
          entity_row(robot, "viewer.measured_tcp", false,
                     member(*tcp, "entity").as_string(),
                     member(*tcp, "source_header_frame_id").as_string(),
                     offset(*tcp, "local_position_xyz", zero),
                     offset(*tcp, "local_orientation_xyzw", identity)));
    }
  }
  const auto by_key = [](const auto &left, const auto &right) {
    return left.first < right.first;
  };
  std::sort(entities.begin(), entities.end(), by_key);
  std::sort(detections.begin(), detections.end(), by_key);
  Array entity_rows, detection_rows;
  for (auto &entry : entities)
    entity_rows.push_back(std::move(entry.second));
  for (auto &entry : detections)
    detection_rows.push_back(std::move(entry.second));
  row.emplace_back("simulation_entities", Value(std::move(entity_rows)));
  row.emplace_back("detection3d_sources", Value(std::move(detection_rows)));
  return Value(Object{{"pose_sources", Value(Array{Value(std::move(row))})}});
}

const QualifiedCapabilityDefinition &
target_capability(std::span<const QualifiedCapabilityDefinition> capabilities,
                  std::string_view capability_id) {
  const auto *builtin = core::find_builtin_capability_definition(capability_id);
  if (!builtin)
    throw std::invalid_argument(std::string(capability_id) +
                                " capability is not qualified by this build");
  for (const auto &candidate : capabilities) {
    if (candidate.value().capability_id.value() == capability_id &&
        candidate.value().capability_definition_revision.value() ==
            builtin->capability_definition_revision)
      return candidate;
  }
  throw std::invalid_argument(std::string(capability_id) +
                              " capability missing from exact source catalog");
}

/// One derived model-free component type: a single target offering a single
/// capability. Its identity is the digest of its shape under `shape_domain`.
struct ModelFreeTypeKind final {
  std::string_view tag;
  std::string_view shape_domain;
  std::string_view target_id;
  std::string_view capability_id;
};
constexpr ModelFreeTypeKind kPoseSourceType{
    "pose_source", "pose-source-component-shape", kPoseTarget, kPoseCapability};
constexpr ModelFreeTypeKind kAiModelType{"ai_model", "ai-model-component-shape",
                                         kAiModelTargetId,
                                         kAiModelCapabilityId};

ActivationPreparedIoType
prepare_model_free_type(const ModelFreeTypeKind &kind,
                        const std::string &component_id, Value config,
                        std::span<const QualifiedCapabilityDefinition>
                            capabilities) {
  const auto &capability = target_capability(capabilities, kind.capability_id);
  Array target_rows{Value(Object{
      {"command_target_id", Value(std::string(kind.target_id))},
      {"model_group_roles", Value(Array{})},
      {"frame_roles", Value(Array{})},
      {"collision_body_roles", Value(Array{})},
      {"resource_roles", Value(Array{})},
      {"capabilities",
       Value(Array{Value(Object{
           {"capability_id", Value(capability.value().capability_id.value())},
           {"capability_definition_revision",
            Value(capability.value()
                      .capability_definition_revision.value())}})})}})};
  Value shape(Object{{"tags", Value(Array{Value(std::string(kind.tag))})},
                     {"model_roles", Value(Array{})},
                     {"model_mounts", Value(Array{})},
                     {"attachment_interfaces", Value(Array{})},
                     {"command_targets", Value(std::move(target_rows))}});
  const auto shape_digest = content_identity::framed_sha256(
      kind.shape_domain, core::kComponentTypeApiVersion, shape);
  const core::ComponentTypeId id("manyforge." + std::string(kind.tag) + "." +
                                 shape_digest.substr(7U));
  auto document = with(
      with(with(shape, "api_version",
                Value(std::string(core::kComponentTypeApiVersion))),
           "component_type_id", Value(id.value())),
      "component_type_revision", Value("sha256:" + std::string(64, '0')));
  const auto *schema =
      build_qualified_closed_schema_registry().find_catalog_schema_family(
          core::CatalogRecordKind::kComponentType);
  if (!schema || !schema->compiled_schema())
    throw std::logic_error("component type schema is unavailable");
  const auto normalized =
      validate_and_normalize_unqualified_artifact_candidate(
          *schema->compiled_schema(),
          parse_strict_json_model_yaml(canonical_json(document)))
          .normalized_root();
  const auto revision = compute_content_identity(
      normalized, {catalog_record_identity_domain("component_type"),
                   std::string(core::kComponentTypeApiVersion),
                   "component_type_revision"});
  document = with(normalized, "component_type_revision", Value(revision));
  const auto bytes = canonical_json(document);
  const core::ContentDigest digest(sha256_digest(bytes));
  const core::ContentAddressedStoreKey key("sha256/" +
                                           digest.value().substr(7U));
  const core::ComponentTypeCatalogRecordRefDTO reference{
      id,
      core::ComponentTypeRevision(revision),
      schema->schema_id(),
      schema->compiled_schema()->schema_revision(),
      digest,
      key};
  std::vector<QualifiedCapabilityDefinition> selected{capability};
  auto type =
      resolve_and_qualify_component_type(reference, key, bytes, selected);
  return {component_id, std::move(type), bytes, std::move(config)};
}

/// `MANYFORGE_AI_[A-Z0-9_]+`, at most 128 bytes: only these host variables
/// reach the Workbench container.
bool api_key_env_name(std::string_view value) {
  constexpr std::string_view prefix = "MANYFORGE_AI_";
  return value.size() > prefix.size() && value.size() <= 128U &&
         value.starts_with(prefix) &&
         std::all_of(value.begin() + prefix.size(), value.end(),
                     [](unsigned char c) {
                       return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                              c == '_';
                     });
}

/// The authored `{base_url, model, api_key_env}` (api_key_env null or absent:
/// no token) as the unit row; refusal `ai_model_configuration_invalid`.
Value ai_model_row(const std::string &component_id, const Value &config) {
  const auto invalid = [] {
    return std::invalid_argument("ai_model_configuration_invalid");
  };
  if (!config.is_object())
    throw invalid();
  const Value *base_url = nullptr, *model = nullptr, *api_key_env = nullptr;
  for (const auto &[name, value] : config.as_object()) {
    if (name == "base_url")
      base_url = &value;
    else if (name == "model")
      model = &value;
    else if (name == "api_key_env")
      api_key_env = &value;
    else
      throw invalid();
  }
  if (!base_url || !base_url->is_string() || !model || !model->is_string())
    throw invalid();
  const auto &url = base_url->as_string();
  const auto scheme = url.starts_with("https://") ? 8U
                      : url.starts_with("http://") ? 7U
                                                   : 0U;
  if (scheme == 0U || url.size() == scheme || url.size() > 2048U ||
      std::any_of(url.begin(), url.end(), [](unsigned char c) {
        return c <= ' ' || c == 0x7f;
      }) ||
      model->as_string().empty() || model->as_string().size() > 256U)
    throw invalid();
  Object row{{"controllable_component_id", Value(component_id)},
             {"base_url", *base_url},
             {"model", *model}};
  if (api_key_env && !api_key_env->is_null()) {
    if (!api_key_env->is_string() ||
        !api_key_env_name(api_key_env->as_string()))
      throw std::invalid_argument("ai_model_api_key_env_invalid");
    row.emplace_back("api_key_env", *api_key_env);
  }
  return Value(Object{{"ai_models", Value(Array{Value(std::move(row))})}});
}
} // namespace

std::vector<ActivationPreparedIoType> prepare_activation_pose_source_types(
    const ActivationDeploymentIntent &intent,
    std::span<const ActivationComponentProfile> profiles,
    const std::vector<ActivationPreparedModel> &prepared_models,
    std::span<const QualifiedCapabilityDefinition> capabilities) {
  if (intent.components.size() != profiles.size())
    throw std::invalid_argument("pose-source component profile join differs");
  const auto *link_rows = intent.policies.find_object_member("scene_pose_sources");
  const auto links = link_rows ? DecodeScenePoseSources(*link_rows)
                               : std::vector<core::ScenePoseSourcePolicyDTO>{};
  const auto *stream_rows =
      intent.policies.find_object_member("component_streams");
  const auto streams =
      stream_rows ? decode_component_stream_overrides(*stream_rows)
                  : std::vector<core::StreamParameterOverrideDTO>{};
  std::map<std::string, const ActivationPreparedModel *, std::less<>>
      models_by_component;
  std::size_t prepared_index = 0U;
  for (const auto &profile : profiles) {
    if (is_model_free_runtime_kind(profile.runtime_kind))
      continue;
    if (prepared_index >= prepared_models.size() ||
        !models_by_component
             .emplace(profile.component_id,
                      &prepared_models[prepared_index++])
             .second)
      throw std::invalid_argument(
          "pose-source component profile/model join differs");
  }
  if (prepared_index != prepared_models.size())
    throw std::invalid_argument(
        "pose-source component profile/model join differs");
  // Every link names a pose-source component of this Deployment. A link whose
  // object is not in the Program is allowed (runtime ignores it).
  for (const auto &link : links)
    if (std::none_of(profiles.begin(), profiles.end(), [&](const auto &row) {
          return row.component_id == link.source_id &&
                 row.runtime_kind == ActivationRuntimeKind::kPoseSource;
        }))
      throw std::invalid_argument("scene_pose_source_unknown_component");
  std::vector<ActivationPreparedIoType> result;
  for (std::size_t index = 0; index != profiles.size(); ++index) {
    const auto &profile = profiles[index];
    const auto &component = intent.components[index];
    if (component.component_id != profile.component_id)
      throw std::invalid_argument(
          "pose-source component profile ordering differs");
    if (profile.runtime_kind != ActivationRuntimeKind::kPoseSource)
      continue;
    auto configuration = normalize_configuration(
        member(component.configuration, "runtime_configuration"));
    const auto references =
        relative_references(configuration, intent, models_by_component);
    configuration = with_relative_references(
        std::move(configuration), references);
    result.push_back(prepare_model_free_type(
        kPoseSourceType, component.component_id,
        expand_pose_source(component.component_id, configuration, links,
                           streams),
        capabilities));
  }
  return result;
}

std::vector<ActivationPreparedIoType> prepare_activation_ai_model_types(
    const ActivationDeploymentIntent &intent,
    std::span<const ActivationComponentProfile> profiles,
    std::span<const QualifiedCapabilityDefinition> capabilities) {
  if (intent.components.size() != profiles.size())
    throw std::invalid_argument("AI model component profile join differs");
  std::vector<ActivationPreparedIoType> result;
  for (std::size_t index = 0; index != profiles.size(); ++index) {
    const auto &component = intent.components[index];
    if (profiles[index].runtime_kind != ActivationRuntimeKind::kAiModel)
      continue;
    result.push_back(prepare_model_free_type(
        kAiModelType, component.component_id,
        ai_model_row(component.component_id,
                     member(component.configuration, "runtime_configuration")),
        capabilities));
  }
  return result;
}
} // namespace manyforge::deployment::detail
